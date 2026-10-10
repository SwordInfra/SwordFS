// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for the split orphan-handoff and private chunk-GC responsibilities.
// A test-only facade runs both independent passes where older end-to-end
// regression scenarios need to observe the complete durable sequence.

#include <fcntl.h>
#include <folly/Synchronized.h>
#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "RedisTestVolumeUtils.hpp"
#include "TestCOWChunkMetadata.hpp"
#include "UnsupportedMetaEngine.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/cow/COWCleanup.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "chunk/internal/ChunkCleanupParticipant.hpp"
#include "chunk/internal/ChunkGcWorker.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Reclaim.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/OrphanReclaimer.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsVolume;
using swordfs::test::UnsupportedMetaEngine;
using swordfs::utils::Status;

metadata::PendingDelete MakePendingDelete(InodeID ino, const SwordFsChunk &descriptor) {
  metadata::PendingDelete pending;
  EXPECT_TRUE(chunk::cow::FreezeCOWDelete(ino, descriptor, 0, &pending).ok());
  return pending;
}

metadata::PendingDelete MakeTypedRevisionDelete(metadata::ChunkID chunk_id, metadata::cow::COWChunkRevision revision) {
  metadata::BufEncoder enc;
  enc.Header(metadata::RecordType::kCowTypedCleanup);
  enc.U32(static_cast<uint32_t>(chunk::cow::COWCleanupKind::kRevision));
  enc.U64(chunk_id.Value());
  enc.U64(revision.Value());
  metadata::PendingDelete pending;
  pending.id = "cow:revision:" + std::to_string(chunk_id.Value()) + ":" + std::to_string(revision.Value());
  enc.Finish(&pending.payload);
  return pending;
}

metadata::PendingDelete MakeTypedDetachedDelete(InodeID ino, metadata::ChunkIndex index, metadata::ChunkID chunk_id) {
  metadata::BufEncoder enc;
  enc.Header(metadata::RecordType::kCowTypedCleanup);
  enc.U32(static_cast<uint32_t>(chunk::cow::COWCleanupKind::kDetachedChunk));
  enc.U64(ino);
  enc.U64(index);
  enc.U64(chunk_id.Value());
  metadata::PendingDelete pending;
  pending.id = "cow:chunk:" + std::to_string(chunk_id.Value());
  enc.Finish(&pending.payload);
  return pending;
}

metadata::ReclaimWork MakeTypedDetachedReclaim(
    InodeID ino, const std::vector<std::pair<metadata::ChunkIndex, metadata::ChunkID>> &chunks) {
  metadata::BufEncoder enc;
  enc.Header(metadata::RecordType::kCowTypedCleanup);
  enc.U32(static_cast<uint32_t>(chunk::cow::COWCleanupKind::kDetachedReclaim));
  enc.U64(ino);
  enc.U32(static_cast<uint32_t>(chunks.size()));
  for (const auto &[index, chunk_id] : chunks) {
    enc.U64(index);
    enc.U64(chunk_id.Value());
  }
  metadata::ReclaimWork work;
  work.ino = ino;
  enc.Finish(&work.payload);
  return work;
}

std::unique_ptr<chunk::internal::ChunkCleanupParticipant> MakeLegacyCleanupParticipant(metadata::IMetaEngine *meta,
                                                                                       storage::IDataEngine *data) {
  if (meta == nullptr || data == nullptr) {
    return nullptr;
  }
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  auto &volume = volume::VolumeImpl::Instance();
  const auto status = chunk::internal::CreateChunkCleanupParticipant(metadata::ChunkType::kCow, volume.chunk_size(), {},
                                                                     meta, data, {}, &cleanup);
  EXPECT_TRUE(status.ok()) << status.message();
  return cleanup;
}

utils::Status ReconcilePhysicalCleanup(storage::IDataEngine *data) {
  auto &volume = volume::VolumeImpl::Instance();
  chunk::internal::ChunkGcWorker worker(volume.meta_engine(), MakeLegacyCleanupParticipant(volume.meta_engine(), data));
  return worker.Reconcile();
}

utils::Status ReconcileAll(storage::IDataEngine *data) {
  auto status = OrphanReclaimer::Instance().Reconcile();
  if (!status.ok()) {
    return status;
  }
  return ReconcilePhysicalCleanup(data);
}

std::unique_ptr<chunk::internal::ChunkGcWorker> MakeChunkGcWorker(storage::IDataEngine *data) {
  auto &volume = volume::VolumeImpl::Instance();
  return std::make_unique<chunk::internal::ChunkGcWorker>(volume.meta_engine(),
                                                          MakeLegacyCleanupParticipant(volume.meta_engine(), data));
}

class CompletingCleanupParticipant final : public chunk::internal::ChunkCleanupParticipant {
 public:
  Status DeletePending(const metadata::PendingDelete &work, bool *completed) override {
    pending_id = work.id;
    *completed = true;
    return Status::OK();
  }

  Status DeleteReclaim(const metadata::ReclaimWork &work, bool *completed) override {
    reclaim_ino = work.ino;
    *completed = true;
    return Status::OK();
  }

  std::string pending_id;
  InodeID reclaim_ino = 0;
};

class DeferredCleanupParticipant final : public chunk::internal::ChunkCleanupParticipant {
 public:
  Status DeletePending(const metadata::PendingDelete &, bool *completed) override {
    *completed = false;
    return Status::OK();
  }

  Status DeleteReclaim(const metadata::ReclaimWork &, bool *completed) override {
    *completed = false;
    return Status::OK();
  }
};

// Data engine that records every Delete and can fail selected keys.
class RecordingDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    state_.wlock()->objects[std::string(key)] =
        std::string(reinterpret_cast<const char *>(data->data()), data->length());
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view key) override {
    const std::string owned(key);
    auto state = state_.wlock();
    state->delete_calls.push_back(owned);
    auto it = state->fail_keys.find(owned);
    if (it != state->fail_keys.end()) {
      // The worker has observed an actual failed Delete, not merely entered
      // the callback. The test also checks that reclaim work stays durable.
      failed_delete_seen_.post();
      return it->second;
    }
    state->objects.erase(owned);
    return Status::OK();
  }

  void Seed(std::string key) {
    state_.wlock()->objects[std::move(key)] = "seeded";
  }
  bool Contains(std::string_view key) const {
    auto state = state_.rlock();
    return state->objects.find(std::string(key)) != state->objects.end();
  }

  void SetDeleteFailure(std::string key, Status status) {
    state_.wlock()->fail_keys[std::move(key)] = std::move(status);
  }
  void ClearDeleteFailures() {
    state_.wlock()->fail_keys.clear();
  }
  std::vector<std::string> DeleteCalls() const {
    return state_.rlock()->delete_calls;
  }
  size_t DeleteCallCount() const {
    return state_.rlock()->delete_calls.size();
  }
  bool WaitForFailedDelete(std::chrono::steady_clock::duration timeout) {
    return failed_delete_seen_.try_wait_for(timeout);
  }

 private:
  struct State {
    std::unordered_map<std::string, std::string> objects;
    std::vector<std::string> delete_calls;
    std::unordered_map<std::string, Status> fail_keys;
  };

  folly::Synchronized<State> state_;
  folly::fibers::Baton failed_delete_seen_;
};

class FaultInjectingCOWChunkMetadata final : public metadata::cow::COWChunkMetadata {
 public:
  Status AllocateChunkID(metadata::ChunkID *out) override {
    return delegate_.AllocateChunkID(out);
  }

  Status AllocateRevision(metadata::ChunkID chunk_id, metadata::cow::COWChunkRevision *out) override {
    return delegate_.AllocateRevision(chunk_id, out);
  }

  Status GetHead(metadata::ChunkID chunk_id, metadata::cow::COWChunkHead *out) override {
    if (!get_head_status_.ok()) {
      return get_head_status_;
    }
    return delegate_.GetHead(chunk_id, out);
  }

  Status CompareExchangeHead(metadata::ChunkID chunk_id, const std::optional<metadata::cow::COWChunkHead> &expected,
                             const metadata::cow::COWChunkHead &replacement) override {
    return delegate_.CompareExchangeHead(chunk_id, expected, replacement);
  }

  Status EraseHead(metadata::ChunkID chunk_id, const metadata::cow::COWChunkHead &expected) override {
    if (!erase_head_status_.ok()) {
      return erase_head_status_;
    }
    return delegate_.EraseHead(chunk_id, expected);
  }

  void SetGetHeadStatus(Status status) {
    get_head_status_ = std::move(status);
  }

  void SetEraseHeadStatus(Status status) {
    erase_head_status_ = std::move(status);
  }

 private:
  swordfs::test::TestCOWChunkMetadata delegate_;
  Status get_head_status_;
  Status erase_head_status_;
};

class StagedIntentMetaEngine : public UnsupportedMetaEngine {
 public:
  explicit StagedIntentMetaEngine(metadata::PendingDelete pending) : pending_(std::move(pending)) {
    chunk::cow::COWRef ref;
    const auto status = chunk::cow::DecodeCOWDelete(*pending_, 0, &ref);
    EXPECT_TRUE(status.ok()) << status.message();
    pending_ino_ = ref.ino;
    current_ = ref.descriptor;
  }

  Status VisitPendingDeletesBatch(size_t max_items, const metadata::PendingDeleteVisitorFn &visitor,
                                  bool *has_more) override {
    if (max_items == 0 || !visitor || has_more == nullptr) {
      return Status::InvalidArgument("invalid pending delete batch request");
    }
    *has_more = false;
    if (!pending_.has_value()) {
      return Status::OK();
    }
    return visitor(*pending_);
  }

  Status CompletePendingDelete(std::string_view key) override {
    if (!pending_.has_value() || key != pending_->id) {
      return Status::NotFound("pending delete not found");
    }
    completed_keys.push_back(std::string(key));
    pending_.reset();
    return Status::OK();
  }

  Status FindChunk(InodeID ino, metadata::ChunkIndex idx, SwordFsChunk *chunk) override {
    if (!find_status_.ok()) {
      return find_status_;
    }
    if (!current_.has_value() || ino != pending_ino_ || idx != current_->index) {
      return Status::NotFound("chunk not found");
    }
    if (chunk != nullptr) {
      *chunk = *current_;
    }
    return Status::OK();
  }

  Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }

  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    return Status::OK();
  }

  void SetCurrent(std::optional<SwordFsChunk> current) {
    current_ = std::move(current);
  }

  void SetFindStatus(Status status) {
    find_status_ = std::move(status);
  }

  std::vector<std::string> completed_keys;

 private:
  InodeID pending_ino_;
  std::optional<metadata::PendingDelete> pending_;
  std::optional<SwordFsChunk> current_;
  Status find_status_ = Status::OK();
};

class RawPendingDeleteMetaEngine : public UnsupportedMetaEngine {
 public:
  explicit RawPendingDeleteMetaEngine(metadata::PendingDelete pending) : pending_(std::move(pending)) {
  }

  Status VisitPendingDeletesBatch(size_t, const metadata::PendingDeleteVisitorFn &visitor, bool *has_more) override {
    *has_more = false;
    return visitor(pending_);
  }
  Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, metadata::ChunkIndex, SwordFsChunk *) override {
    return Status::NotFound("no authoritative chunk");
  }

 private:
  metadata::PendingDelete pending_;
};

class PendingReclaimMetaEngine : public UnsupportedMetaEngine {
 public:
  explicit PendingReclaimMetaEngine(metadata::ReclaimWork frozen) : frozen_(std::move(frozen)) {
  }

  Status VisitPendingDeletesBatch(size_t, const metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    *has_more = false;
    return Status::OK();
  }
  Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &visitor) override {
    return completed ? Status::OK() : visitor(frozen_);
  }
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status GetInode(InodeID file_ino, SwordFsInode *out) override {
    if (!get_inode_status_.ok()) {
      return get_inode_status_;
    }
    if (file_ino == frozen_.ino) {
      if (inode_live_) {
        if (out != nullptr) {
          out->ino = file_ino;
        }
        return Status::OK();
      }
      return Status::NotFound("inode already removed");
    }
    return UnsupportedMetaEngine::GetInode(file_ino, out);
  }
  Status CompleteReclaim(InodeID file_ino) override {
    if (file_ino != frozen_.ino) {
      return Status::NotFound("reclaim inode mismatch");
    }
    completed = true;
    return Status::OK();
  }

  void SetInodeLive(bool live) {
    inode_live_ = live;
  }

  void SetGetInodeStatus(Status status) {
    get_inode_status_ = std::move(status);
  }

  bool completed = false;

 private:
  metadata::ReclaimWork frozen_;
  bool inode_live_ = false;
  Status get_inode_status_ = Status::OK();
};

class ForcedMultiPassMetaEngine : public UnsupportedMetaEngine {
 public:
  ForcedMultiPassMetaEngine() {
    constexpr InodeID kProbeIno = 42;
    descriptors_[0] = SwordFsChunk{.index = 0, .revision = 7, .size = 64};
    descriptors_[1] = SwordFsChunk{.index = 1, .revision = 8, .size = 64};
    pending_[0] = MakePendingDelete(kProbeIno, descriptors_[0]);
    pending_[1] = MakePendingDelete(kProbeIno, descriptors_[1]);
    probe_ino_ = kProbeIno;
  }

  Status VisitPendingDeletesBatch(size_t max_items, const metadata::PendingDeleteVisitorFn &visitor,
                                  bool *has_more) override {
    if (max_items == 0 || !visitor || has_more == nullptr) {
      return Status::InvalidArgument("invalid forced pending-delete batch request");
    }

    const int scan = pending_delete_scan_calls_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const size_t item_index = scan == 1 ? 0 : 1;
    auto status = visitor(pending_[item_index]);
    if (!status.ok()) {
      return status;
    }

    *has_more = scan == 1;
    if (scan == 2) {
      second_scan_followed_orphan_.store(orphan_scan_calls_.load(std::memory_order_acquire) > 0,
                                         std::memory_order_release);
      second_pending_scan_started_.post();
    }
    return Status::OK();
  }

  Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }

  Status CompletePendingDelete(std::string_view) override {
    // This test controls the two scan passes explicitly. Acknowledgement is
    // not part of its scheduling contract and must not block the second pass.
    return Status::OK();
  }

  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    orphan_scan_calls_.fetch_add(1, std::memory_order_release);
    return Status::OK();
  }

  Status FindChunk(InodeID ino, metadata::ChunkIndex idx, SwordFsChunk *chunk) override {
    if (ino != probe_ino_ || idx >= descriptors_.size()) {
      return Status::NotFound("probe chunk not found");
    }
    if (chunk != nullptr) {
      *chunk = descriptors_[idx];
    }
    return Status::OK();
  }

  bool WaitForSecondPendingScan(std::chrono::steady_clock::duration timeout) {
    return second_pending_scan_started_.try_wait_for(timeout);
  }

  int pending_delete_scan_calls() const {
    return pending_delete_scan_calls_.load(std::memory_order_acquire);
  }

  bool second_scan_followed_orphan() const {
    return second_scan_followed_orphan_.load(std::memory_order_acquire);
  }

 private:
  InodeID probe_ino_ = 0;
  std::array<SwordFsChunk, 2> descriptors_;
  std::array<metadata::PendingDelete, 2> pending_;
  std::atomic<int> pending_delete_scan_calls_{0};
  std::atomic<int> orphan_scan_calls_{0};
  std::atomic<bool> second_scan_followed_orphan_{false};
  folly::fibers::Baton second_pending_scan_started_;
};

class EmptyReclaimerMetaEngine : public UnsupportedMetaEngine {
 public:
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingDeletesBatch(size_t, const metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }
};

class ReclaimerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SwordFsVolume config;
    auto meta = CreateMetaEngine(&config);
    ASSERT_NE(meta, nullptr);
    auto data = std::make_unique<RecordingDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    // A test that started the reclaim worker must never let it outlive the
    // engines it walks; Stop() is idempotent and safe when never started.
    OrphanReclaimer::Instance().Stop();
    // Drop per-inode runtime state and the injected engines on the threads
    // that own them.
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  virtual std::unique_ptr<metadata::IMetaEngine> CreateMetaEngine(SwordFsVolume *) {
    return std::make_unique<swordfs::test::ConfiguredMetaEngine<EmptyReclaimerMetaEngine>>();
  }

  Status ReadInodeStatus(InodeID ino) {
    SwordFsInode inode;
    return meta_->GetInode(ino, &inode);
  }

  // Create a regular file with one published chunk, and seed the matching
  // object in the data engine. Returns the inode id.
  InodeID CreateChunkedFile(std::string_view name, uint64_t revision = 1) {
    SwordFsInode file;
    auto status = meta_->Create(kRootInodeId, name, 0644, &file);
    EXPECT_TRUE(status.ok()) << status.message();
    SwordFsChunk chunk{.index = 0, .revision = revision, .size = 64};
    status = meta_->CommitChunk(file.ino, std::nullopt, chunk);
    EXPECT_TRUE(status.ok()) << status.message();
    data_->Seed(chunk::cow::FormatCOWObjectKey(file.ino, 0, revision));
    return file.ino;
  }

  std::vector<InodeID> OrphanCandidates() {
    std::vector<InodeID> out;
    auto status = meta_->VisitOrphanCandidates([&out](InodeID ino) {
      out.push_back(ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    std::sort(out.begin(), out.end());
    return out;
  }

  std::vector<InodeID> PendingReclaims() {
    std::vector<InodeID> out;
    auto status = meta_->VisitPendingReclaims([&out](const metadata::ReclaimWork &work) {
      out.push_back(work.ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    std::sort(out.begin(), out.end());
    return out;
  }

  std::vector<std::string> PendingDeletes() {
    std::vector<std::string> out;
    bool has_more = false;
    auto status = meta_->VisitPendingDeletesBatch(
        1024,
        [&out](const metadata::PendingDelete &work) {
          chunk::cow::COWRef ref;
          auto status = chunk::cow::DecodeCOWDelete(work, volume::VolumeImpl::Instance().chunk_size(), &ref);
          if (!status.ok()) {
            return status;
          }
          out.push_back(ref.key);
          return Status::OK();
        },
        &has_more);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_FALSE(has_more);
    return out;
  }

  metadata::IMetaEngine *meta_ = nullptr;
  RecordingDataEngine *data_ = nullptr;
};

class ReclaimerRedisTest : public ReclaimerTest {
 protected:
  void SetUp() override {
    if (std::getenv("SWORDFS_REDIS_TEST_URL") == nullptr) {
      GTEST_SKIP() << "Redis metadata service is not configured";
    }
    ReclaimerTest::SetUp();
  }

  std::unique_ptr<metadata::IMetaEngine> CreateMetaEngine(SwordFsVolume *config) override {
    std::unique_ptr<metadata::RedisMetaImpl> engine;
    const auto status = swordfs::test::MakeFormattedRedisMetaEngine("reclaimer", config, &engine);
    EXPECT_TRUE(status.ok()) << status.message();
    return engine;
  }
};

FIBER_TEST_F(ReclaimerTest, ChunkCleanupFactoryRejectsUnsupportedMechanism) {
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  const auto status = chunk::internal::CreateChunkCleanupParticipant(
      metadata::ChunkType::kChunkSlice, volume::VolumeImpl::Instance().chunk_size(), {}, meta_, data_, {}, &cleanup);
  EXPECT_EQ(status.ToErrno(), ENOSYS);
  EXPECT_EQ(cleanup, nullptr);
}

FIBER_TEST_F(ReclaimerTest, ChunkCleanupFactoryRejectsInvalidConstruction) {
  EXPECT_EQ(chunk::internal::CreateChunkCleanupParticipant(metadata::ChunkType::kCow, 0, {}, meta_, data_, {}, nullptr)
                .ToErrno(),
            EINVAL);

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  EXPECT_EQ(chunk::cow::CreateCOWCleanupParticipant(0, {}, meta_, data_, {}, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::CreateCOWCleanupParticipant(0, {}, nullptr, data_, {}, &cleanup).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::CreateCOWCleanupParticipant(0, {}, meta_, nullptr, {}, &cleanup).ToErrno(), EINVAL);
}

FIBER_TEST_F(ReclaimerTest, TypedCOWCleanupRejectsMalformedPersistedIdentity) {
  constexpr InodeID kIno = 42;
  constexpr metadata::ChunkIndex kIndex = 3;
  auto chunk_metadata = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, {}, &cleanup).ok());

  const auto expect_bad_pending = [&](metadata::PendingDelete pending) {
    bool completed = true;
    const auto status = cleanup->DeletePending(pending, &completed);
    EXPECT_EQ(status.ToErrno(), EIO) << status.message();
    EXPECT_FALSE(completed);
  };
  const auto expect_bad_reclaim = [&](metadata::ReclaimWork work) {
    bool completed = true;
    const auto status = cleanup->DeleteReclaim(work, &completed);
    EXPECT_EQ(status.ToErrno(), EIO) << status.message();
    EXPECT_FALSE(completed);
  };

  metadata::PendingDelete missing_kind{.id = "cow:typed:missing-kind"};
  metadata::BufEncoder missing_kind_encoder;
  missing_kind_encoder.Header(metadata::RecordType::kCowTypedCleanup);
  missing_kind_encoder.Finish(&missing_kind.payload);
  expect_bad_pending(std::move(missing_kind));

  expect_bad_pending(MakeTypedRevisionDelete(metadata::kInvalidChunkID, revision));
  expect_bad_pending(MakeTypedRevisionDelete(chunk_id, metadata::cow::kInvalidCOWChunkRevision));
  expect_bad_pending(
      MakeTypedRevisionDelete(chunk_id, metadata::cow::COWChunkRevision(metadata::cow::kMaxCOWChunkRevisionValue + 1)));

  auto mismatched_revision_id = MakeTypedRevisionDelete(chunk_id, revision);
  mismatched_revision_id.id += ":tampered";
  expect_bad_pending(std::move(mismatched_revision_id));

  expect_bad_pending(MakeTypedDetachedDelete(0, kIndex, chunk_id));
  expect_bad_pending(MakeTypedDetachedDelete(kIno, kIndex, metadata::kInvalidChunkID));

  auto mismatched_detached_id = MakeTypedDetachedDelete(kIno, kIndex, chunk_id);
  mismatched_detached_id.id += ":tampered";
  expect_bad_pending(std::move(mismatched_detached_id));

  metadata::PendingDelete invalid_kind{.id = "cow:typed:invalid-kind"};
  metadata::BufEncoder invalid_kind_encoder;
  invalid_kind_encoder.Header(metadata::RecordType::kCowTypedCleanup);
  invalid_kind_encoder.U32(999);
  invalid_kind_encoder.Finish(&invalid_kind.payload);
  expect_bad_pending(std::move(invalid_kind));

  auto mismatched_reclaim_ino = MakeTypedDetachedReclaim(kIno, {});
  mismatched_reclaim_ino.ino = kIno + 1;
  expect_bad_reclaim(std::move(mismatched_reclaim_ino));

  expect_bad_reclaim(MakeTypedDetachedReclaim(kIno, {{kIndex, metadata::kInvalidChunkID}}));

  metadata::ReclaimWork trailing_reclaim;
  trailing_reclaim.ino = kIno;
  metadata::BufEncoder trailing_encoder;
  trailing_encoder.Header(metadata::RecordType::kCowTypedCleanup);
  trailing_encoder.U32(static_cast<uint32_t>(chunk::cow::COWCleanupKind::kDetachedReclaim));
  trailing_encoder.U64(kIno);
  trailing_encoder.U32(0);
  trailing_encoder.U64(123);
  trailing_encoder.Finish(&trailing_reclaim.payload);
  expect_bad_reclaim(std::move(trailing_reclaim));

  // A corrupt queue entry must not reserve billions of elements before
  // discovering that its payload contains no detached identities.
  metadata::ReclaimWork overstated_reclaim;
  overstated_reclaim.ino = kIno;
  metadata::BufEncoder overstated_encoder;
  overstated_encoder.Header(metadata::RecordType::kCowTypedCleanup);
  overstated_encoder.U32(static_cast<uint32_t>(chunk::cow::COWCleanupKind::kDetachedReclaim));
  overstated_encoder.U64(kIno);
  overstated_encoder.U32(std::numeric_limits<uint32_t>::max());
  overstated_encoder.Finish(&overstated_reclaim.payload);
  expect_bad_reclaim(std::move(overstated_reclaim));

  EXPECT_EQ(cleanup->DeletePending(MakeTypedRevisionDelete(chunk_id, revision), nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(cleanup->DeleteReclaim(MakeTypedDetachedReclaim(kIno, {}), nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(ReclaimerTest, TypedCOWCleanupRequiresConfiguredAuthority) {
  constexpr InodeID kIno = 42;
  constexpr metadata::ChunkIndex kIndex = 3;
  auto chunk_metadata = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, {}, meta_, data_, {}, &cleanup).ok());
  bool completed = false;
  EXPECT_EQ(cleanup->DeletePending(MakeTypedRevisionDelete(chunk_id, revision), &completed).ToErrno(), ENOSYS);

  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, {}, &cleanup).ok());
  completed = false;
  EXPECT_EQ(cleanup->DeletePending(MakeTypedDetachedDelete(kIno, kIndex, chunk_id), &completed).ToErrno(), ENOSYS);
}

FIBER_TEST_F(ReclaimerTest, TypedCOWRevisionCleanupFailsClosedOnMetadataAndDeleteErrors) {
  auto chunk_metadata = std::make_shared<FaultInjectingCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, {}, &cleanup).ok());
  const auto pending = MakeTypedRevisionDelete(chunk_id, revision);

  chunk_metadata->SetGetHeadStatus(Status::IOError("head lookup failed"));
  bool completed = false;
  EXPECT_EQ(cleanup->DeletePending(pending, &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->DeleteCalls().empty());

  chunk_metadata->SetGetHeadStatus(Status::OK());
  const chunk::cow::COWObjectKey key(chunk_id, revision);
  data_->Seed(std::string(static_cast<std::string_view>(key)));
  data_->SetDeleteFailure(std::string(static_cast<std::string_view>(key)), Status::IOError("delete failed"));
  EXPECT_EQ(cleanup->DeletePending(pending, &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));
}

FIBER_TEST_F(ReclaimerTest, TypedCOWDetachedCleanupFailsClosedOnAuthorityAndStorageErrors) {
  constexpr InodeID kFile_ino = 42;
  constexpr metadata::ChunkIndex kIndex = 3;
  auto chunk_metadata = std::make_shared<FaultInjectingCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());
  const metadata::cow::COWChunkHead head{revision, 64};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, std::nullopt, head).ok());
  const chunk::cow::COWObjectKey key(chunk_id, revision);
  data_->Seed(std::string(static_cast<std::string_view>(key)));

  chunk::internal::ChunkReachabilityProbeFn failing_probe = [](InodeID, metadata::ChunkIndex,
                                                               std::optional<metadata::ChunkID> *) {
    return Status::IOError("reachability lookup failed");
  };
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, failing_probe, &cleanup).ok());
  bool completed = false;
  EXPECT_EQ(cleanup->DeletePending(MakeTypedDetachedDelete(kFile_ino, kIndex, chunk_id), &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->DeleteCalls().empty());

  chunk::internal::ChunkReachabilityProbeFn detached_probe = [](InodeID, metadata::ChunkIndex,
                                                                std::optional<metadata::ChunkID> *out) {
    out->reset();
    return Status::OK();
  };
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, {}, meta_, data_, detached_probe, &cleanup).ok());
  EXPECT_EQ(cleanup->DeletePending(MakeTypedDetachedDelete(kFile_ino, kIndex, chunk_id), &completed).ToErrno(), ENOSYS);

  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, detached_probe, &cleanup).ok());
  metadata::ChunkID headless_chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&headless_chunk_id).ok());
  completed = false;
  EXPECT_TRUE(cleanup->DeletePending(MakeTypedDetachedDelete(kFile_ino, kIndex, headless_chunk_id), &completed).ok());
  EXPECT_TRUE(completed);

  chunk_metadata->SetGetHeadStatus(Status::IOError("head lookup failed"));
  completed = false;
  EXPECT_EQ(cleanup->DeletePending(MakeTypedDetachedDelete(kFile_ino, kIndex, chunk_id), &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));

  chunk_metadata->SetGetHeadStatus(Status::OK());
  data_->SetDeleteFailure(std::string(static_cast<std::string_view>(key)), Status::IOError("delete failed"));
  EXPECT_EQ(cleanup->DeletePending(MakeTypedDetachedDelete(kFile_ino, kIndex, chunk_id), &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));

  data_->ClearDeleteFailures();
  chunk_metadata->SetEraseHeadStatus(Status::IOError("erase head failed"));
  EXPECT_EQ(cleanup->DeletePending(MakeTypedDetachedDelete(kFile_ino, kIndex, chunk_id), &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  metadata::cow::COWChunkHead current;
  EXPECT_TRUE(chunk_metadata->GetHead(chunk_id, &current).ok());
}

FIBER_TEST_F(ReclaimerTest, TypedCOWReclaimPropagatesReachabilityAndDeleteFailures) {
  constexpr InodeID kFile_ino = 42;
  constexpr metadata::ChunkIndex kIndex = 3;
  auto chunk_metadata = std::make_shared<FaultInjectingCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());
  const metadata::cow::COWChunkHead head{revision, 64};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, std::nullopt, head).ok());
  const chunk::cow::COWObjectKey key(chunk_id, revision);
  data_->Seed(std::string(static_cast<std::string_view>(key)));
  const auto work = MakeTypedDetachedReclaim(kFile_ino, {{kIndex, chunk_id}});

  chunk::internal::ChunkReachabilityProbeFn failing_probe = [](InodeID, metadata::ChunkIndex,
                                                               std::optional<metadata::ChunkID> *) {
    return Status::IOError("reachability lookup failed");
  };
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, failing_probe, &cleanup).ok());
  bool completed = false;
  EXPECT_EQ(cleanup->DeleteReclaim(work, &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->DeleteCalls().empty());

  chunk::internal::ChunkReachabilityProbeFn detached_probe = [](InodeID, metadata::ChunkIndex,
                                                                std::optional<metadata::ChunkID> *out) {
    out->reset();
    return Status::OK();
  };
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, detached_probe, &cleanup).ok());
  data_->SetDeleteFailure(std::string(static_cast<std::string_view>(key)), Status::IOError("delete failed"));
  EXPECT_EQ(cleanup->DeleteReclaim(work, &completed).ToErrno(), EIO);
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));
}

FIBER_TEST_F(ReclaimerTest, ChunkGcRejectsMalformedPendingDeleteWork) {
  const metadata::PendingDelete malformed{.id = "malformed", .payload = "not-a-COW-delete"};

  RawPendingDeleteMetaEngine *raw_meta = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<RawPendingDeleteMetaEngine>>(malformed);
    raw_meta = replacement.get();
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  chunk::internal::ChunkGcWorker worker(raw_meta, MakeLegacyCleanupParticipant(raw_meta, data_));
  const auto status = worker.Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_TRUE(data_->DeleteCalls().empty());
}

FIBER_TEST_F(ReclaimerTest, ChunkGcDelegatesReclaimPolicyAndOwnsAcknowledgement) {
  constexpr InodeID kIno = 42;
  const metadata::ReclaimWork frozen{.ino = kIno, .payload = "participant-owned-opaque-payload"};

  PendingReclaimMetaEngine *pending_meta = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PendingReclaimMetaEngine>>(frozen);
    pending_meta = replacement.get();
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  auto cleanup = std::make_unique<CompletingCleanupParticipant>();
  auto *cleanup_ptr = cleanup.get();
  chunk::internal::ChunkGcWorker worker(pending_meta, std::move(cleanup));
  EXPECT_TRUE(worker.Reconcile().ok());
  EXPECT_EQ(cleanup_ptr->reclaim_ino, kIno);
  EXPECT_TRUE(pending_meta->completed);
}

FIBER_TEST_F(ReclaimerTest, ChunkGcKeepsDeferredReclaimUnacknowledged) {
  constexpr InodeID kIno = 42;
  const metadata::ReclaimWork frozen{.ino = kIno, .payload = "participant-owned-opaque-payload"};

  PendingReclaimMetaEngine *pending_meta = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PendingReclaimMetaEngine>>(frozen);
    pending_meta = replacement.get();
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  chunk::internal::ChunkGcWorker worker(pending_meta, std::make_unique<DeferredCleanupParticipant>());
  EXPECT_TRUE(worker.Reconcile().ok());
  EXPECT_FALSE(pending_meta->completed);
}

FIBER_TEST_F(ReclaimerTest, ChunkGcRejectsMalformedReclaimWork) {
  const metadata::ReclaimWork malformed{.ino = 42, .payload = "not-a-COW-reclaim"};

  PendingReclaimMetaEngine *pending_meta = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PendingReclaimMetaEngine>>(malformed);
    pending_meta = replacement.get();
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  const auto status = ReconcilePhysicalCleanup(data_);
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_FALSE(pending_meta->completed);
}

FIBER_TEST_F(ReclaimerTest, ChunkGcFailsClosedWhenReclaimReachabilityLookupFails) {
  constexpr InodeID kIno = 42;
  const SwordFsChunk descriptor{.index = 0, .revision = 7, .size = 64};
  metadata::ReclaimWork frozen;
  ASSERT_TRUE(chunk::cow::FreezeCOWReclaim(kIno, {descriptor}, 0, &frozen).ok());

  PendingReclaimMetaEngine *pending_meta = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PendingReclaimMetaEngine>>(frozen);
    pending_meta = replacement.get();
    pending_meta->SetGetInodeStatus(Status::IOError("inode lookup unavailable"));
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  const auto status = ReconcilePhysicalCleanup(data_);
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_FALSE(pending_meta->completed);
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileDeletesFrozenObjectsAndCompletes) {
  const InodeID f_ino = CreateChunkedFile("f");
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  ASSERT_TRUE(ReconcileAll(data_).ok());

  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  EXPECT_EQ(data_->DeleteCalls(), std::vector<std::string>{key});
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound());
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_TRUE(OrphanCandidates().empty());

  // Nothing left to do: a second pass is a no-op.
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_EQ(data_->DeleteCallCount(), 1U);

  // The prepared output is optional; omitting it must not change the reclaim
  // sequence or leave the frozen record behind.
  const InodeID second = CreateChunkedFile("second", 2);
  const auto second_key = chunk::cow::FormatCOWObjectKey(second, 0, 2);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "second").ok());
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_FALSE(data_->Contains(second_key));
  EXPECT_TRUE(ReadInodeStatus(second).IsNotFound());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(ReclaimerTest, FrozenWorkDoesNotAuthorizeDeletingALiveInodeAndMayBeDiscarded) {
  constexpr InodeID kFileIno = 42;
  const SwordFsChunk head{.index = 0, .revision = 7, .size = 64};
  metadata::ReclaimWork frozen;
  ASSERT_TRUE(chunk::cow::FreezeCOWReclaim(kFileIno, {head}, 0, &frozen).ok());

  PendingReclaimMetaEngine *pending_meta = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PendingReclaimMetaEngine>>(frozen);
    pending_meta = replacement.get();
    pending_meta->SetInodeLive(true);
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto load_status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(load_status.ok()) << load_status.message();
  });

  const auto key = chunk::cow::FormatCOWObjectKey(kFileIno, 0, 7);
  data_->Seed(key);

  const auto live_status = ReconcilePhysicalCleanup(data_);
  EXPECT_TRUE(live_status.ok()) << live_status.message();
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_TRUE(pending_meta->completed);

  // The maintenance handoff was stale and has been acknowledged. If logical
  // state later becomes unreachable, losing that old target is allowed to
  // leak storage rather than turning stale queue state into future authority.
  pending_meta->SetInodeLive(false);
  ASSERT_TRUE(ReconcilePhysicalCleanup(data_).ok());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->DeleteCalls().empty());
}

TEST_F(ReclaimerTest, PendingReclaimContinuesDirectlyToDeletion) {
  constexpr InodeID kFileIno = 42;
  const SwordFsChunk head{.index = 0, .revision = 7, .size = 64};
  metadata::ReclaimWork frozen;
  ASSERT_TRUE(chunk::cow::FreezeCOWReclaim(kFileIno, {head}, 0, &frozen).ok());
  auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PendingReclaimMetaEngine>>(frozen);
  auto *pending_meta = replacement.get();
  auto data = std::make_unique<RecordingDataEngine>();
  data_ = data.get();
  SwordFsVolume config;
  const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
  ASSERT_TRUE(status.ok()) << status.message();
  const auto key = chunk::cow::FormatCOWObjectKey(kFileIno, 0, 7);
  data_->Seed(key);

  swordfs::test::RunInTestFiber([&] { ASSERT_TRUE(ReconcileAll(data_).ok()); });
  EXPECT_TRUE(pending_meta->completed);
  EXPECT_FALSE(data_->Contains(key));
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileRetriesFailedObjectDeletes) {
  const InodeID f_ino = CreateChunkedFile("f");
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  data_->SetDeleteFailure(key, Status::IOError("injected delete failure"));

  const auto first = ReconcileAll(data_);
  EXPECT_FALSE(first.ok());
  EXPECT_EQ(data_->DeleteCallCount(), 1U);
  // The frozen record survives the failed delete...
  EXPECT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  // ... and reconciliation completes it (idempotently) once the backend
  // recovers.
  data_->ClearDeleteFailures();
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_EQ(data_->DeleteCallCount(), 2U);
  EXPECT_EQ(data_->DeleteCalls()[1], key);
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound());
}

FIBER_TEST_F(ReclaimerRedisTest, TruncateCleanupDoesNotDependOnLocalChunkCache) {
  const InodeID f_ino = CreateChunkedFile("truncate");
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);

  // Call the authoritative metadata engine directly: this deliberately skips
  // FileReadWriter/FileChunkManager, modelling a cold cache or a fresh mount.
  ASSERT_TRUE(meta_->Truncate(f_ino, 0).ok());
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{key});
  EXPECT_TRUE(data_->Contains(key));

  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(PendingDeletes().empty());
}

FIBER_TEST_F(ReclaimerRedisTest, TruncateCleanupRetriesFailedDelete) {
  const InodeID f_ino = CreateChunkedFile("truncate-retry");
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Truncate(f_ino, 0).ok());
  data_->SetDeleteFailure(key, Status::IOError("injected delete failure"));

  EXPECT_EQ(ReconcileAll(data_).ToErrno(), EIO);
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{key});
  EXPECT_TRUE(data_->Contains(key));

  data_->ClearDeleteFailures();
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_TRUE(PendingDeletes().empty());
  EXPECT_FALSE(data_->Contains(key));
}

FIBER_TEST_F(ReclaimerRedisTest, RewriteCleanupDeletesOnlySupersededRevision) {
  const InodeID f_ino = CreateChunkedFile("rewrite");
  SwordFsChunk first;
  ASSERT_TRUE(meta_->FindChunk(f_ino, 0, &first).ok());

  auto replacement = first;
  replacement.revision = first.revision + 1;
  const auto old_key = chunk::cow::FormatCOWObjectKey(f_ino, first.index, first.revision);
  const auto new_key = chunk::cow::FormatCOWObjectKey(f_ino, replacement.index, replacement.revision);
  data_->Seed(new_key);

  ASSERT_TRUE(meta_->CommitChunk(f_ino, first, replacement).ok());
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{old_key});
  EXPECT_TRUE(data_->Contains(old_key));
  EXPECT_TRUE(data_->Contains(new_key));

  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_FALSE(data_->Contains(old_key));
  EXPECT_TRUE(data_->Contains(new_key));
  EXPECT_TRUE(PendingDeletes().empty());

  SwordFsChunk authoritative;
  ASSERT_TRUE(meta_->FindChunk(f_ino, 0, &authoritative).ok());
  EXPECT_EQ(authoritative, replacement);
}

FIBER_TEST_F(ReclaimerTest, TypedCOWRevisionCleanupUsesChunkIDRevisionAndIgnoresHeadSize) {
  auto chunk_metadata = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());

  metadata::cow::COWChunkHead head{revision, 64};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, std::nullopt, head).ok());
  const chunk::cow::COWObjectKey key(chunk_id, revision);
  data_->Seed(std::string(static_cast<std::string_view>(key)));

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, {}, &cleanup).ok());
  const auto pending = MakeTypedRevisionDelete(chunk_id, revision);

  bool completed = true;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));

  // Size is full-head CAS state, not immutable-object identity. The same
  // revision remains live after a size-only shrink.
  const metadata::cow::COWChunkHead resized{revision, 32};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, head, resized).ok());
  completed = true;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_FALSE(completed);
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));

  metadata::cow::COWChunkRevision replacement_revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &replacement_revision).ok());
  const metadata::cow::COWChunkHead replacement{replacement_revision, 32};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, resized, replacement).ok());
  const chunk::cow::COWObjectKey replacement_key(chunk_id, replacement_revision);
  data_->Seed(std::string(static_cast<std::string_view>(replacement_key)));

  completed = false;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_TRUE(completed);
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(key)));
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(replacement_key)));
}

FIBER_TEST_F(ReclaimerTest, TypedCOWDetachedCleanupRequiresFileMetadataUnreachability) {
  constexpr InodeID kIno = 42;
  constexpr metadata::ChunkIndex kIndex = 3;
  auto chunk_metadata = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&chunk_id).ok());
  metadata::cow::COWChunkRevision revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &revision).ok());
  const metadata::cow::COWChunkHead head{revision, 64};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, std::nullopt, head).ok());

  const chunk::cow::COWObjectKey key(chunk_id, revision);
  data_->Seed(std::string(static_cast<std::string_view>(key)));
  std::optional<metadata::ChunkID> attached = chunk_id;
  chunk::internal::ChunkReachabilityProbeFn probe = [&](InodeID ino, metadata::ChunkIndex index,
                                                        std::optional<metadata::ChunkID> *out) {
    EXPECT_EQ(ino, kIno);
    EXPECT_EQ(index, kIndex);
    *out = attached;
    return Status::OK();
  };

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, probe, &cleanup).ok());
  const auto pending = MakeTypedDetachedDelete(kIno, kIndex, chunk_id);

  bool completed = false;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_TRUE(completed) << "an attached candidate is stale maintenance, not future delete authority";
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(key)));
  metadata::cow::COWChunkHead current;
  EXPECT_TRUE(chunk_metadata->GetHead(chunk_id, &current).ok());

  attached.reset();
  completed = false;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_TRUE(completed);
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(key)));
  EXPECT_TRUE(chunk_metadata->GetHead(chunk_id, &current).IsNotFound());

  // Detaching never resets either identity allocator. A late private update
  // can create unreachable residue, but it cannot revive the ChunkID.
  metadata::cow::COWChunkRevision late_revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(chunk_id, &late_revision).ok());
  EXPECT_GT(late_revision.Value(), revision.Value());
  const metadata::cow::COWChunkHead late_head{late_revision, 16};
  ASSERT_TRUE(chunk_metadata->CompareExchangeHead(chunk_id, std::nullopt, late_head).ok());
  const chunk::cow::COWObjectKey late_key(chunk_id, late_revision);
  data_->Seed(std::string(static_cast<std::string_view>(late_key)));

  completed = false;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_TRUE(completed);
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(late_key)));

  metadata::ChunkID next_chunk_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&next_chunk_id).ok());
  EXPECT_GT(next_chunk_id.Value(), chunk_id.Value());
}

FIBER_TEST_F(ReclaimerTest, TypedCOWReclaimPrevalidatesEveryDetachedChunkBeforeDeletion) {
  constexpr InodeID kIno = 42;
  auto chunk_metadata = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID first_id;
  metadata::ChunkID second_id;
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&first_id).ok());
  ASSERT_TRUE(chunk_metadata->AllocateChunkID(&second_id).ok());
  metadata::cow::COWChunkRevision first_revision;
  metadata::cow::COWChunkRevision second_revision;
  ASSERT_TRUE(chunk_metadata->AllocateRevision(first_id, &first_revision).ok());
  ASSERT_TRUE(chunk_metadata->AllocateRevision(second_id, &second_revision).ok());
  ASSERT_TRUE(
      chunk_metadata->CompareExchangeHead(first_id, std::nullopt, metadata::cow::COWChunkHead{first_revision, 64})
          .ok());
  ASSERT_TRUE(
      chunk_metadata->CompareExchangeHead(second_id, std::nullopt, metadata::cow::COWChunkHead{second_revision, 64})
          .ok());
  const chunk::cow::COWObjectKey first_key(first_id, first_revision);
  const chunk::cow::COWObjectKey second_key(second_id, second_revision);
  data_->Seed(std::string(static_cast<std::string_view>(first_key)));
  data_->Seed(std::string(static_cast<std::string_view>(second_key)));

  std::optional<metadata::ChunkID> first_attached;
  std::optional<metadata::ChunkID> second_attached = second_id;
  chunk::internal::ChunkReachabilityProbeFn probe = [&](InodeID ino, metadata::ChunkIndex index,
                                                        std::optional<metadata::ChunkID> *out) {
    EXPECT_EQ(ino, kIno);
    *out = index == 0 ? first_attached : second_attached;
    return Status::OK();
  };
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(0, chunk_metadata, meta_, data_, probe, &cleanup).ok());
  const auto work = MakeTypedDetachedReclaim(kIno, {{0, first_id}, {1, second_id}});

  bool completed = false;
  ASSERT_TRUE(cleanup->DeleteReclaim(work, &completed).ok());
  EXPECT_TRUE(completed);
  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(first_key)));
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(second_key)));

  second_attached.reset();
  completed = false;
  ASSERT_TRUE(cleanup->DeleteReclaim(work, &completed).ok());
  EXPECT_TRUE(completed);
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(first_key)));
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(second_key)));
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteCandidateDoesNotDeleteStillAuthoritativeObject) {
  constexpr InodeID kIno = 42;
  SwordFsChunk descriptor{.index = 0, .revision = 7, .size = 64};
  const auto key = chunk::cow::FormatCOWObjectKey(kIno, descriptor.index, descriptor.revision);
  metadata::PendingDelete pending = MakePendingDelete(kIno, descriptor);

  StagedIntentMetaEngine *staged = nullptr;
  // Metadata-engine construction/destruction is control-plane work. Replace
  // the fixture's metadata engine on a POSIX thread so the Debug execution-domain
  // contract remains identical to production lifecycle.
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<StagedIntentMetaEngine>>(pending);
    staged = staged_meta.get();
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(staged_meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  data_->Seed(key);

  // Queue membership alone is never delete authority. Reconciliation must
  // leave both object and candidate untouched while the exact immutable key
  // is still authoritative.
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_TRUE(staged->completed_keys.empty());

  // Once authoritative metadata no longer names that immutable object, the
  // same candidate becomes executable and is acknowledged only after deletion.
  staged->SetCurrent(std::nullopt);
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_EQ(data_->DeleteCalls(), std::vector<std::string>{key});
  EXPECT_EQ(staged->completed_keys, std::vector<std::string>{pending.id});
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteFailsClosedWhenAuthoritativeChunkLookupFails) {
  constexpr InodeID kIno = 42;
  SwordFsChunk descriptor{.index = 0, .revision = 7, .size = 64};
  const auto key = chunk::cow::FormatCOWObjectKey(kIno, descriptor.index, descriptor.revision);
  metadata::PendingDelete pending = MakePendingDelete(kIno, descriptor);

  StagedIntentMetaEngine *staged = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<StagedIntentMetaEngine>>(pending);
    staged = staged_meta.get();
    staged->SetFindStatus(Status::IOError("chunk lookup unavailable"));
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto load_status =
        swordfs::test::LoadTestVolumeRuntime(std::move(staged_meta), std::move(data), std::move(config));
    ASSERT_TRUE(load_status.ok()) << load_status.message();
  });
  data_->Seed(key);

  const auto status = ReconcileAll(data_);
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_TRUE(staged->completed_keys.empty());
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteRemovesSupersededRevisionWhileNewRevisionStaysAuthoritative) {
  constexpr InodeID kIno = 42;
  SwordFsChunk old_descriptor{.index = 0, .revision = 7, .size = 64};
  const auto old_key = chunk::cow::FormatCOWObjectKey(kIno, old_descriptor.index, old_descriptor.revision);
  metadata::PendingDelete pending = MakePendingDelete(kIno, old_descriptor);

  StagedIntentMetaEngine *staged = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<StagedIntentMetaEngine>>(pending);
    staged = staged_meta.get();
    staged->SetCurrent(SwordFsChunk{.index = 0, .revision = 8, .size = 64});
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(staged_meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  data_->Seed(old_key);

  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_FALSE(data_->Contains(old_key));
  EXPECT_EQ(data_->DeleteCalls(), std::vector<std::string>{old_key});
  EXPECT_EQ(staged->completed_keys, std::vector<std::string>{pending.id});
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileRecoversCrashLeftOrphan) {
  // Crash model: the unlink published the orphan candidate and the process
  // died before any reclaim ran. A fresh mount has no descriptors and no
  // handle state, so mount-time reconciliation is exactly what runs here.
  const InodeID f_ino = CreateChunkedFile("f");
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  ASSERT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});

  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  EXPECT_TRUE(data_->Contains(key)) << "nothing may be deleted before the reclaim is prepared";

  ASSERT_TRUE(ReconcileAll(data_).ok());

  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileLeavesRevivedInodeAlone) {
  const InodeID f_ino = CreateChunkedFile("f");
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());

  // A hard link revives the inode before reconciliation runs (the marker is
  // dropped by the link transaction itself).
  SwordFsInode revived;
  ASSERT_TRUE(meta_->Link(f_ino, kRootInodeId, "revived", &revived).ok());
  EXPECT_TRUE(OrphanCandidates().empty());

  ASSERT_TRUE(ReconcileAll(data_).ok());

  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  EXPECT_TRUE(data_->DeleteCalls().empty()) << "no object may be deleted while a name references the inode";
  EXPECT_TRUE(data_->Contains(key));
  ASSERT_TRUE(meta_->GetInode(f_ino, &revived).ok());
  EXPECT_EQ(revived.attr.nlink, 1U);
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileDefersUntilAfterTheLastDescriptorCloses) {
  const InodeID f_ino = CreateChunkedFile("f");

  // Open a descriptor, then unlink: the reclaim must be deferred while the
  // descriptor lives, and the inode must survive for it.
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(f_ino, O_RDONLY, &handle).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());

  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_TRUE(data_->DeleteCalls().empty()) << "an open descriptor must defer the cleanup";
  ASSERT_TRUE(ReadInodeStatus(f_ino).ok());

  // Close only releases the local reference; background reclaim remains the
  // sole executor and completes on the next pass.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_TRUE(data_->DeleteCalls().empty());
  ASSERT_TRUE(ReconcileAll(data_).ok());
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound());

  // Once reclaimed, the inode cannot be reopened.
  std::shared_ptr<FileHandle> reopened;
  EXPECT_TRUE(FileHandle::Open(f_ino, O_RDONLY, &reopened).IsNotFound());
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileDefersAnUnlinkedInodeWithAnOpenDescriptor) {
  // #143: the background reconciliation is a reclaim caller like any other, so
  // it must go through the same per-inode decision. While a descriptor holds
  // the unlinked inode open, a full Reconcile pass may not delete the inode or
  // its objects.
  const InodeID f_ino = CreateChunkedFile("f");
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(f_ino, O_RDONLY, &handle).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  ASSERT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});

  ASSERT_TRUE(ReconcileAll(data_).ok());

  // Nothing was reclaimed: the candidate is still durable, the inode still
  // resolves, and the object the open descriptor can still read is untouched.
  EXPECT_TRUE(data_->DeleteCalls().empty()) << "reconciliation must not reclaim an inode a descriptor still holds";
  EXPECT_TRUE(data_->Contains(key));
  ASSERT_TRUE(ReadInodeStatus(f_ino).ok()) << "the inode must survive while the descriptor is open";
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{f_ino}));
  EXPECT_TRUE(PendingReclaims().empty());

  // Last close only releases the local reference. The next worker pass owns
  // preparation/deletion/completion.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_TRUE(data_->DeleteCalls().empty());
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_EQ(data_->DeleteCalls(), (std::vector<std::string>{key}));
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
}

// ────────────────────────────────────────────────────────────────
// #141: a reclaim must never delete objects a Link can still revive
// ────────────────────────────────────────────────────────────────
// The link and the reclaim race on the same inode. Exactly one outcome is
// legal: the link revived the inode — and then its chunk metadata and its
// object must both still be there — or the reclaim prepared first — and then
// the link must have failed and the object must be gone. A revived inode
// whose object was deleted would be silent data loss.

FIBER_TEST_F(ReclaimerRedisTest, ConcurrentReclaimAndLinkNeverDeleteLiveData) {
  constexpr int kRounds = 100;

  std::atomic<int> revived{0};
  std::atomic<int> reclaimed{0};
  std::atomic<int> failures{0};

  for (int round = 0; round < kRounds; ++round) {
    const InodeID f_ino = CreateChunkedFile("race");
    const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
    ASSERT_TRUE(meta_->Unlink(kRootInodeId, "race").ok());

    std::barrier gate(3);
    std::atomic<bool> link_won{false};

    auto linker = swordfs::test::StartFiberTestThread([&] {
      gate.arrive_and_wait();
      SwordFsInode inode;
      if (meta_->Link(f_ino, kRootInodeId, "revived", &inode).ok()) {
        link_won.store(true);
      }
    });
    auto reclaimer = swordfs::test::StartFiberTestThread([&] {
      gate.arrive_and_wait();
      if (!ReconcileAll(data_).ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    });

    gate.arrive_and_wait();
    linker.join();
    reclaimer.join();

    if (link_won.load()) {
      // The revival won: the inode, its chunk metadata and its object are
      // all still there.
      SwordFsChunk chunk;
      EXPECT_TRUE(meta_->FindChunk(f_ino, 0, &chunk).ok()) << "round " << round << ": revived inode lost its metadata";
      EXPECT_TRUE(data_->Contains(key)) << "round " << round << ": revived inode lost its object";
      revived.fetch_add(1, std::memory_order_relaxed);
      // Clean up the fresh orphan before the next round.
      ASSERT_TRUE(meta_->Unlink(kRootInodeId, "revived").ok());
      ASSERT_TRUE(ReconcileAll(data_).ok());
    } else {
      // The reclaim won: the inode is gone and so is its object.
      EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound()) << "round " << round;
      EXPECT_FALSE(data_->Contains(key)) << "round " << round << ": reclaimed inode left its object behind";
      reclaimed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  EXPECT_EQ(revived.load() + reclaimed.load(), kRounds);
  EXPECT_EQ(failures.load(), 0);
}

// ────────────────────────────────────────────────────────────────
// #143: a reclaim with no engines must fail closed, and a
// reconciliation with no engines must not touch anything
// ────────────────────────────────────────────────────────────────
// VolumeImpl::Initialize() leaves both planes absent — the state a mount
// reaches before LoadFrom() binds the engines. Nothing may be reported as
// reclaimed in that state, and a recovery pass must stay harmless.

class ReclaimerNoEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    volume::VolumeImpl::Initialize();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    OrphanReclaimer::Instance().Stop();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }
};

FIBER_TEST_F(ReclaimerNoEngineTest, ReconcileWithoutEnginesIsANoOp) {
  EXPECT_TRUE(ReconcileAll(nullptr).ok());
}

FIBER_TEST_F(ReclaimerNoEngineTest, MissingDataEngineFailsClosedAndReconcileStaysHarmless) {
  swordfs::test::RunInTestThreadFromFiber([&] {
    SwordFsVolume config;
    auto configured = std::make_unique<swordfs::test::ConfiguredMetaEngine<EmptyReclaimerMetaEngine>>();
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(configured), nullptr, std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  ASSERT_NE(volume::VolumeImpl::Instance().meta_engine(), nullptr);
  EXPECT_TRUE(ReconcileAll(nullptr).ok());
}

// ────────────────────────────────────────────────────────────────
// #143/#142: one failing cleanup item must not hide the rest of the work
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(ReclaimerRedisTest, ReconcileCountsEveryFailedCleanupItem) {
  // One truncate pending-delete, one orphan candidate and one already-frozen
  // pending reclaim all fail. The pass must attempt all three and report the
  // aggregate — a failure in one durable queue must not hide another.
  const InodeID truncated = CreateChunkedFile("truncated");
  const InodeID orphan = CreateChunkedFile("orphan");
  const InodeID frozen = CreateChunkedFile("frozen");
  const auto truncated_key = chunk::cow::FormatCOWObjectKey(truncated, 0, 1);
  const auto orphan_key = chunk::cow::FormatCOWObjectKey(orphan, 0, 1);
  const auto frozen_key = chunk::cow::FormatCOWObjectKey(frozen, 0, 1);
  ASSERT_TRUE(meta_->Truncate(truncated, 0).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "orphan").ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "frozen").ok());

  // Freeze one directly through metadata to model a crash-left pending record
  // without involving the worker path under test.
  ASSERT_TRUE(meta_->PrepareReclaim(frozen).ok());
  data_->SetDeleteFailure(frozen_key, Status::IOError("injected delete failure"));
  ASSERT_EQ(PendingReclaims(), std::vector<InodeID>{frozen});

  data_->SetDeleteFailure(truncated_key, Status::IOError("injected delete failure"));
  data_->SetDeleteFailure(orphan_key, Status::IOError("injected delete failure"));
  const auto status = ReconcileAll(data_);
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_NE(status.message().find("left 3 cleanup item(s) pending"), std::string::npos) << status.message();

  // All three durable records survive for the next pass.
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{truncated_key});
  EXPECT_EQ(PendingReclaims(), (std::vector<InodeID>{orphan, frozen}));
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(data_->Contains(truncated_key));
  EXPECT_TRUE(data_->Contains(orphan_key));
  EXPECT_TRUE(data_->Contains(frozen_key));

  // With the backend healthy the next pass finishes all work: the failed pass
  // lost nothing.
  data_->ClearDeleteFailures();
  ASSERT_TRUE(ReconcileAll(data_).ok());
  EXPECT_TRUE(PendingDeletes().empty());
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_FALSE(data_->Contains(truncated_key));
  EXPECT_FALSE(data_->Contains(orphan_key));
  EXPECT_FALSE(data_->Contains(frozen_key));
  EXPECT_TRUE(ReadInodeStatus(truncated).ok());
  EXPECT_TRUE(ReadInodeStatus(orphan).IsNotFound());
  EXPECT_TRUE(ReadInodeStatus(frozen).IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// #143: a backend that cannot answer the scan must be reported
// ────────────────────────────────────────────────────────────────
// Reconcile() walks two durable sets; a backend failure on either scan must
// reach the caller instead of being read as an empty set.

class FailingScanMetaEngine : public UnsupportedMetaEngine {
 public:
  Status VisitPendingDeletesBatch(size_t, const swordfs::metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (!pending_delete_scan_status.ok()) {
      return pending_delete_scan_status;
    }
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }

  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &visitor) override {
    if (!orphan_scan_status.ok()) {
      return orphan_scan_status;
    }
    if (candidate_ino_.has_value()) {
      return visitor(*candidate_ino_);
    }
    return Status::OK();
  }

  Status VisitPendingReclaims(const swordfs::metadata::ReclaimVisitorFn &) override {
    if (!pending_scan_status.ok()) {
      return pending_scan_status;
    }
    return Status::OK();
  }

  Status PrepareReclaim(InodeID) override {
    if (!prepare_reclaim_status.ok()) {
      return prepare_reclaim_status;
    }
    return Status::OK();
  }

  void SetCandidate(InodeID ino) {
    candidate_ino_ = ino;
  }

  Status orphan_scan_status = Status::OK();
  Status pending_scan_status = Status::OK();
  Status pending_delete_scan_status = Status::OK();
  Status prepare_reclaim_status = Status::OK();

 private:
  std::optional<InodeID> candidate_ino_;
};

class ReclaimerScanFailureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<FailingScanMetaEngine>>();
    auto data = std::make_unique<RecordingDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    SwordFsVolume config;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  FailingScanMetaEngine *meta_ = nullptr;
  RecordingDataEngine *data_ = nullptr;
};

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesOrphanScanFailure) {
  meta_->orphan_scan_status = Status::IOError("orphan scan unavailable");

  const auto status = ReconcileAll(data_);
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(status.message(), "orphan scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileAggregatesPrepareReclaimFailure) {
  // Only the scanner's visitor result matters to this worker test; the
  // namespace operation itself is covered by the real Redis tests.
  meta_->SetCandidate(42);
  meta_->prepare_reclaim_status = Status::IOError("prepare reclaim unavailable");

  const auto status = OrphanReclaimer::Instance().Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(status.message(), "orphan reclaim left 1 inode(s) pending");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesPendingDeleteScanFailure) {
  meta_->pending_delete_scan_status = Status::IOError("pending delete scan unavailable");

  const auto status = ReconcileAll(data_);
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(status.message(), "pending delete scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesPendingScanFailure) {
  // The orphan scan is healthy; the second visitor's failure must reach the
  // caller just the same.
  meta_->pending_scan_status = Status::IOError("pending scan unavailable");

  const auto status = ReconcileAll(data_);
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(status.message(), "pending scan unavailable");
}

// ────────────────────────────────────────────────────────────────
// #143: the reclaim worker
// ────────────────────────────────────────────────────────────────
// Production starts it at mount init and joins it at teardown — both
// POSIX-thread hooks — while each pass is fiber-domain work handed to that
// thread's own fiber runtime. The thread's effect is the only observable, so
// these tests watch the durable record rather than the thread.

FIBER_TEST_F(ReclaimerRedisTest, WorkerCompletesAPendingReclaimImmediatelyAtStartup) {
  const InodeID f_ino = CreateChunkedFile("f");
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());

  ASSERT_TRUE(meta_->PrepareReclaim(f_ino).ok());
  ASSERT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  auto worker = MakeChunkGcWorker(data_);
  swordfs::test::RunInTestThreadFromFiber([&] { worker->Start(); });

  // Start() runs the first pass immediately; give the worker room to hand the
  // pass to its fiber runtime and complete it.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !PendingReclaims().empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const bool completed = PendingReclaims().empty();

  swordfs::test::RunInTestThreadFromFiber([&] { worker->Stop(); });

  EXPECT_TRUE(completed) << "the startup worker pass must finish crash-left reclaim work";
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(f_ino).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileLeavesLargeOrphanBacklogsForLaterPassesAndEventuallyDrainsThem) {
  constexpr int kCandidates = 300;
  for (int i = 0; i < kCandidates; ++i) {
    SwordFsInode file;
    const std::string name = "orphan-batch-" + std::to_string(i);
    ASSERT_TRUE(meta_->Create(kRootInodeId, name, 0644, &file).ok());
    ASSERT_TRUE(meta_->Unlink(kRootInodeId, name).ok());
  }
  ASSERT_EQ(OrphanCandidates().size(), static_cast<size_t>(kCandidates));

  ASSERT_TRUE(OrphanReclaimer::Instance().Reconcile().ok());
  const auto after_first_pass = OrphanCandidates();
  EXPECT_FALSE(after_first_pass.empty()) << "one reconciliation pass must not monopolize an unbounded orphan backlog";
  EXPECT_LT(after_first_pass.size(), static_cast<size_t>(kCandidates))
      << "a bounded pass must still make forward progress";

  for (int pass = 0; pass < 10 && !OrphanCandidates().empty(); ++pass) {
    ASSERT_TRUE(OrphanReclaimer::Instance().Reconcile().ok());
  }
  EXPECT_TRUE(OrphanCandidates().empty()) << "repeated bounded passes must converge on the durable backlog";
  EXPECT_EQ(PendingReclaims().size(), static_cast<size_t>(kCandidates));
}

class ReclaimerForcedMultiPassTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<ForcedMultiPassMetaEngine>>();
    auto data = std::make_unique<RecordingDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    SwordFsVolume config;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    OrphanReclaimer::Instance().Stop();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  ForcedMultiPassMetaEngine *meta_ = nullptr;
  RecordingDataEngine *data_ = nullptr;
};

class SlowOrphanScanMetaEngine : public UnsupportedMetaEngine {
 public:
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &visitor) override {
    constexpr int kCandidates = 1000;
    for (int i = 0; i < kCandidates; ++i) {
      scan_started_.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      auto status = visitor(static_cast<InodeID>(1000 + i));
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }

  Status PrepareReclaim(InodeID) override {
    return Status::OK();
  }

  bool scan_started() const {
    return scan_started_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool> scan_started_{false};
};

class ReclaimerSlowScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<SlowOrphanScanMetaEngine>>();
    meta_ = meta.get();
    SwordFsVolume config;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::make_unique<RecordingDataEngine>(),
                                                             std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    OrphanReclaimer::Instance().Stop();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  SlowOrphanScanMetaEngine *meta_ = nullptr;
};

TEST_F(ReclaimerForcedMultiPassTest, ChunkGcWorkerSelfWakesWithoutScanningVfsOrphans) {
  // The production safety scan is five seconds. This shorter mechanism-level
  // watchdog proves has_more -> Wake() started pass two rather than the
  // periodic fallback; the number of pending items says nothing about the
  // production batch width.
  constexpr auto kBeforeSafetyFallbackWatchdog = std::chrono::seconds(4);

  auto worker = MakeChunkGcWorker(data_);
  worker->Start();
  const bool second_pass_started = meta_->WaitForSecondPendingScan(kBeforeSafetyFallbackWatchdog);
  worker->Stop();

  EXPECT_TRUE(second_pass_started) << "has_more must self-wake the worker before the periodic safety scan";
  EXPECT_GE(meta_->pending_delete_scan_calls(), 2);
  EXPECT_FALSE(meta_->second_scan_followed_orphan())
      << "private chunk GC must not scan or depend on the VFS orphan queue";
}

TEST_F(ReclaimerSlowScanTest, OrphanWorkerStopInterruptsALongCandidateScan) {
  auto &worker = OrphanReclaimer::Instance();
  worker.Start();

  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < start_deadline && !meta_->scan_started()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(meta_->scan_started()) << "the worker must enter the orphan scan before shutdown is measured";

  const auto started = std::chrono::steady_clock::now();
  worker.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // A mount with a large durable orphan backlog must still tear down promptly.
  // Stop should interrupt candidate visitation instead of joining an entire
  // backlog scan before the daemon can exit.
  EXPECT_LT(elapsed, std::chrono::milliseconds(250));
}

FIBER_TEST_F(ReclaimerRedisTest, WorkerSurvivesAFailedPassAndRecoversOnWake) {
  const InodeID f_ino = CreateChunkedFile("retry-failure");
  const auto key = chunk::cow::FormatCOWObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "retry-failure").ok());

  // Freeze the reclaim first, and keep the backend failing so the first worker
  // pass reports an error instead of completing the record.
  ASSERT_TRUE(meta_->PrepareReclaim(f_ino).ok());
  data_->SetDeleteFailure(key, Status::IOError("injected persistent delete failure"));
  ASSERT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  auto worker = MakeChunkGcWorker(data_);
  swordfs::test::RunInTestThreadFromFiber([&] { worker->Start(); });

  // Observe an actual failed Delete result rather than racing against the
  // worker's public call log. Durable reclaim state proves the failed
  // attempt cannot have completed deletion.
  const bool failed_delete_observed = data_->WaitForFailedDelete(std::chrono::seconds(15));
  EXPECT_TRUE(failed_delete_observed) << "the worker must attempt the failing delete";
  EXPECT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  // Recover the backend and explicitly wake the same worker; the failed pass
  // must not terminate it and wakeup must not wait for the safety interval.
  data_->ClearDeleteFailures();
  worker->Wake();
  const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < recovery_deadline && !PendingReclaims().empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const bool recovered = PendingReclaims().empty();

  swordfs::test::RunInTestThreadFromFiber([&] { worker->Stop(); });

  EXPECT_TRUE(recovered) << "a failed worker pass must not terminate the reclaim worker";
  EXPECT_FALSE(data_->Contains(key));
}

TEST_F(ReclaimerTest, OrphanWorkerLifecycleIsIdempotent) {
  auto &worker = OrphanReclaimer::Instance();
  const auto started = std::chrono::steady_clock::now();
  worker.Stop();
  worker.Start();
  worker.Start();
  worker.Wake();
  worker.Wake();
  worker.Stop();
  worker.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
}

FIBER_TEST_F(ReclaimerTest, WorkerLifecycleIsIdempotent) {
  // Stopping before any start is a no-op, a second start must not leave a
  // second thread behind, and a stop must return promptly instead of waiting
  // out the retry interval.
  auto worker = MakeChunkGcWorker(data_);
  const auto started = std::chrono::steady_clock::now();
  swordfs::test::RunInTestThreadFromFiber([&] {
    worker->Stop();  // no thread was ever started
    worker->Start();
    worker->Start();  // idempotent
    worker->Stop();
    worker->Stop();  // already joined
  });
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // Stop posts the cross-domain waiter, so teardown should not pay either the
  // five-second retry interval or a polling-slice delay. Keep a generous CI
  // bound while still pinning the prompt-shutdown contract.
  EXPECT_LT(elapsed, std::chrono::milliseconds(500)) << "Stop must wake the chunk GC worker promptly";
}

}  // namespace
}  // namespace swordfs::vfs
