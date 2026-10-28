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
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "RedisTestVolumeUtils.hpp"
#include "TestCOWChunkMetadata.hpp"
#include "UnsupportedMetaEngine.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/cow/COWChunk.hpp"
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
#include "vfs/FileReadWriter.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/OrphanReclaimer.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsVolume;
using swordfs::test::UnsupportedMetaEngine;
using swordfs::utils::Status;

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

std::unique_ptr<chunk::internal::ChunkCleanupParticipant> MakeCleanupParticipant(
    metadata::IMetaEngine *meta, storage::IDataEngine *data, metadata::ChunkMetadataPtr cow_metadata = {}) {
  if (meta == nullptr || data == nullptr) {
    return nullptr;
  }
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  auto &volume = volume::VolumeImpl::Instance();
  // Real Redis tests pass the already-opened private ChunkMetadata from
  // fixture setup. Opening it here would cross the thread/fiber boundary;
  // generic mock tests deliberately use a lightweight in-memory double.
  if (!cow_metadata) {
    cow_metadata = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  }
  auto probe = [meta](InodeID ino, metadata::ChunkIndex index, std::optional<metadata::ChunkID> *out) {
    auto status = meta->ProbeAttachment(ino, index, out);
    if (status.IsNotFound()) {
      out->reset();
      return Status::OK();
    }
    return status;
  };
  const auto status = chunk::internal::CreateChunkCleanupParticipant(metadata::ChunkType::kCow, volume.chunk_size(),
                                                                     cow_metadata, meta, data, probe, &cleanup);
  EXPECT_TRUE(status.ok()) << status.message();
  return cleanup;
}

utils::Status ReconcilePhysicalCleanup(storage::IDataEngine *data, metadata::ChunkMetadataPtr cow_metadata = {}) {
  auto &volume = volume::VolumeImpl::Instance();
  chunk::internal::ChunkGcWorker worker(volume.meta_engine(),
                                        MakeCleanupParticipant(volume.meta_engine(), data, std::move(cow_metadata)));
  return worker.Reconcile();
}

utils::Status ReconcileAll(storage::IDataEngine *data, metadata::ChunkMetadataPtr cow_metadata = {}) {
  auto status = OrphanReclaimer::Instance().Reconcile();
  if (!status.ok()) {
    return status;
  }
  return ReconcilePhysicalCleanup(data, std::move(cow_metadata));
}

std::unique_ptr<chunk::internal::ChunkGcWorker> MakeChunkGcWorker(storage::IDataEngine *data) {
  auto &volume = volume::VolumeImpl::Instance();
  return std::make_unique<chunk::internal::ChunkGcWorker>(volume.meta_engine(),
                                                          MakeCleanupParticipant(volume.meta_engine(), data));
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
  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) override {
    if (get_started_ != nullptr) {
      // Suspend before reading the object: concurrent GC must not physically
      // erase this immutable revision while its reader is in flight.
      auto *started = std::exchange(get_started_, nullptr);
      auto *release = std::exchange(get_release_, nullptr);
      started->post();
      release->wait();
    }
    if (out == nullptr || out->tailroom() < size) {
      return Status::InvalidArgument("invalid recorded data read");
    }
    auto state = state_.rlock();
    const auto it = state->objects.find(std::string(key));
    if (it == state->objects.end() || offset > it->second.size() || size > it->second.size() - offset) {
      return Status::IOError("recorded immutable object unavailable");
    }
    std::memcpy(out->writableTail(), it->second.data() + offset, size);
    out->append(size);
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

  void BlockNextGet(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    get_started_ = started;
    get_release_ = release;
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
  folly::fibers::Baton *get_started_ = nullptr;
  folly::fibers::Baton *get_release_ = nullptr;
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

// A GC queue fixture that deliberately does not implement any legacy
// descriptor API. Malformed typed work must be rejected before data deletion.
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
    pending_[0] = MakeTypedDetachedDelete(kProbeIno, 0, metadata::ChunkID(7));
    pending_[1] = MakeTypedDetachedDelete(kProbeIno, 1, metadata::ChunkID(8));
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
    // Keep the COW cleanup and reclaim fixtures independent of the new
    // default for newly formatted user volumes.
    config.chunk_type = metadata::ChunkType::kCow;
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
    if (!HasFatalFailure()) {
      ASSERT_TRUE(meta_->OpenChunkMetadata(metadata::ChunkType::kCow, &typed_metadata_).ok());
    }
  }

  // Build real FileMetadata attachment and COW-private state, never a
  // synthetic inode/index/revision descriptor from the retired protocol.
  std::pair<InodeID, std::string> CreateTypedChunkFile(std::string_view name) {
    SwordFsInode file;
    auto status = meta_->Create(kRootInodeId, name, 0644, &file);
    if (!status.ok()) {
      ADD_FAILURE() << status.message();
      return {};
    }

    FileReadWriter writer(file.ino, 2);
    status = writer.Write(*folly::IOBuf::copyBuffer("hello"), 0);
    if (status.ok()) {
      status = writer.Flush();
    }
    if (!status.ok()) {
      ADD_FAILURE() << status.message();
      return {};
    }

    metadata::FileChunkSnapshot attached;
    status = meta_->ReadFileChunkSnapshot(file.ino, 0, &attached);
    if (!status.ok() || !attached.chunk_id.has_value()) {
      ADD_FAILURE() << "typed chunk attachment not published";
      return {};
    }
    const auto cow_metadata = std::dynamic_pointer_cast<metadata::cow::COWChunkMetadata>(typed_metadata_);
    if (!cow_metadata) {
      ADD_FAILURE() << "COW-private metadata capability missing";
      return {};
    }
    metadata::cow::COWChunkHead head;
    status = cow_metadata->GetHead(*attached.chunk_id, &head);
    if (!status.ok()) {
      ADD_FAILURE() << status.message();
      return {};
    }
    const chunk::cow::COWObjectKey key(*attached.chunk_id, head.revision);
    return {file.ino, std::string(static_cast<std::string_view>(key))};
  }

  metadata::ChunkMetadataPtr typed_metadata_;

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

FIBER_TEST_F(ReclaimerTest, TypedCleanupFreezeRejectsInvalidIdentityWithoutPublishingWork) {
  metadata::ReclaimWork reclaim{.ino = 77, .payload = "unchanged"};
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedReclaim(0, {}, &reclaim).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedReclaim(42, {}, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedReclaim(42, {{.index = 1, .chunk_id = metadata::kInvalidChunkID}}, &reclaim)
                .ToErrno(),
            EINVAL);
  EXPECT_EQ(reclaim.ino, 77U);
  EXPECT_EQ(reclaim.payload, "unchanged");

  metadata::PendingDelete pending{.id = "unchanged", .payload = "unchanged"};
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedDelete(0, {.index = 1, .chunk_id = metadata::ChunkID(17)}, &pending).ToErrno(),
            EINVAL);
  EXPECT_EQ(
      chunk::cow::FreezeCOWDetachedDelete(42, {.index = 1, .chunk_id = metadata::kInvalidChunkID}, &pending).ToErrno(),
      EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedDelete(42, {.index = 1, .chunk_id = metadata::ChunkID(17)}, nullptr).ToErrno(),
            EINVAL);
  EXPECT_EQ(pending.id, "unchanged");
  EXPECT_EQ(pending.payload, "unchanged");
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
    config.chunk_type = metadata::ChunkType::kCow;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  chunk::internal::ChunkGcWorker worker(raw_meta, MakeCleanupParticipant(raw_meta, data_));
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
    config.chunk_type = metadata::ChunkType::kCow;
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
    config.chunk_type = metadata::ChunkType::kCow;
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
    config.chunk_type = metadata::ChunkType::kCow;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });

  const auto status = ReconcilePhysicalCleanup(data_);
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_FALSE(pending_meta->completed);
}

FIBER_TEST_F(ReclaimerRedisTest, TypedRewriteFlushRegistersAndWorkerDeletesOldRevision) {
  SwordFsInode file;
  ASSERT_TRUE(meta_->Create(kRootInodeId, "typed-gc-rewrite", 0644, &file).ok());
  FileReadWriter writer(file.ino, 2);
  ASSERT_TRUE(writer.Write(*folly::IOBuf::copyBuffer("hello"), 0).ok());
  ASSERT_TRUE(writer.Flush().ok());

  metadata::FileChunkSnapshot first;
  ASSERT_TRUE(meta_->ReadFileChunkSnapshot(file.ino, 0, &first).ok());
  ASSERT_TRUE(first.chunk_id.has_value());
  const auto typed = std::dynamic_pointer_cast<metadata::cow::COWChunkMetadata>(typed_metadata_);
  ASSERT_NE(typed, nullptr);
  metadata::cow::COWChunkHead original;
  ASSERT_TRUE(typed->GetHead(*first.chunk_id, &original).ok());
  const chunk::cow::COWObjectKey old_key(*first.chunk_id, original.revision);
  ASSERT_TRUE(data_->Contains(static_cast<std::string_view>(old_key)));

  ASSERT_TRUE(writer.Write(*folly::IOBuf::copyBuffer("HELLO"), 0).ok());
  ASSERT_TRUE(writer.Flush().ok());
  metadata::cow::COWChunkHead updated;
  ASSERT_TRUE(typed->GetHead(*first.chunk_id, &updated).ok());
  const chunk::cow::COWObjectKey new_key(*first.chunk_id, updated.revision);
  EXPECT_NE(updated.revision, original.revision);

  size_t queued = 0;
  bool has_more = false;
  ASSERT_TRUE(meta_
                  ->VisitPendingDeletesBatch(
                      16,
                      [&](const metadata::PendingDelete &pending) {
                        EXPECT_EQ(pending.id, "cow:revision:" + std::to_string(first.chunk_id->Value()) + ":" +
                                                  std::to_string(original.revision.Value()));
                        ++queued;
                        return Status::OK();
                      },
                      &has_more)
                  .ok());
  EXPECT_EQ(queued, 1U);
  EXPECT_FALSE(has_more);

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  const auto probe = [this](InodeID ino, metadata::ChunkIndex index, std::optional<metadata::ChunkID> *out) {
    return meta_->ProbeAttachment(ino, index, out);
  };
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(volume::VolumeImpl::Instance().chunk_size(), typed_metadata_,
                                                      meta_, data_, probe, &cleanup)
                  .ok());
  chunk::internal::ChunkGcWorker worker(meta_, std::move(cleanup));
  ASSERT_TRUE(worker.Reconcile().ok());
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(old_key)));
  EXPECT_TRUE(data_->Contains(static_cast<std::string_view>(new_key)));
}

FIBER_TEST_F(ReclaimerRedisTest, TypedReclaimRecoversCrashLeftOrphan) {
  const auto [ino, key] = CreateTypedChunkFile("crash-left-orphan");
  ASSERT_NE(ino, 0U);
  ASSERT_TRUE(data_->Contains(key));
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "crash-left-orphan").ok());
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));

  ASSERT_TRUE(ReconcileAll(data_, typed_metadata_).ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(ino).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(ReclaimerRedisTest, TypedReclaimNeverDeletesRevivedInode) {
  const auto [ino, key] = CreateTypedChunkFile("revive-before-reclaim");
  ASSERT_NE(ino, 0U);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "revive-before-reclaim").ok());
  SwordFsInode revived;
  ASSERT_TRUE(meta_->Link(ino, kRootInodeId, "revived", &revived).ok());
  ASSERT_TRUE(ReconcileAll(data_, typed_metadata_).ok());

  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_TRUE(data_->Contains(key));
  ASSERT_TRUE(meta_->GetInode(ino, &revived).ok());
  EXPECT_EQ(revived.attr.nlink, 1U);
  EXPECT_TRUE(OrphanCandidates().empty());
}

FIBER_TEST_F(ReclaimerRedisTest, TypedReclaimWaitsForTheLastOpenDescriptor) {
  const auto [ino, key] = CreateTypedChunkFile("open-during-reclaim");
  ASSERT_NE(ino, 0U);
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(ino, O_RDONLY, &handle).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "open-during-reclaim").ok());
  ASSERT_TRUE(ReconcileAll(data_, typed_metadata_).ok());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->DeleteCalls().empty());
  EXPECT_TRUE(ReadInodeStatus(ino).ok());
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));

  ASSERT_TRUE(handle->Release().ok());
  ASSERT_TRUE(ReconcileAll(data_, typed_metadata_).ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(ino).IsNotFound());
  std::shared_ptr<FileHandle> reopened;
  EXPECT_TRUE(FileHandle::Open(ino, O_RDONLY, &reopened).IsNotFound());
}

FIBER_TEST_F(ReclaimerRedisTest, TypedReclaimRetriesFailedPhysicalDelete) {
  const auto [ino, key] = CreateTypedChunkFile("retry-typed-gc");
  ASSERT_NE(ino, 0U);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "retry-typed-gc").ok());
  data_->SetDeleteFailure(key, Status::IOError("transient delete failure"));
  EXPECT_EQ(ReconcileAll(data_, typed_metadata_).ToErrno(), EIO);
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(ReadInodeStatus(ino).IsNotFound());
  EXPECT_EQ(PendingReclaims(), (std::vector<InodeID>{ino}));

  data_->ClearDeleteFailures();
  ASSERT_TRUE(ReconcileAll(data_, typed_metadata_).ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(ReclaimerRedisTest, TypedLinkAndReclaimRaceCannotDeleteLiveObject) {
  constexpr int kRounds = 12;
  for (int round = 0; round < kRounds; ++round) {
    const auto [ino, key] = CreateTypedChunkFile("reclaim-race");
    ASSERT_NE(ino, 0U);
    ASSERT_TRUE(meta_->Unlink(kRootInodeId, "reclaim-race").ok());

    std::barrier gate(3);
    std::atomic<bool> link_won{false};
    std::atomic<bool> reconcile_failed{false};
    auto linker = swordfs::test::StartFiberTestThread([&] {
      gate.arrive_and_wait();
      SwordFsInode revived;
      link_won.store(meta_->Link(ino, kRootInodeId, "revived-race", &revived).ok());
    });
    auto reclaimer = swordfs::test::StartFiberTestThread([&] {
      gate.arrive_and_wait();
      reconcile_failed.store(!ReconcileAll(data_, typed_metadata_).ok());
    });
    gate.arrive_and_wait();
    linker.join();
    reclaimer.join();
    ASSERT_FALSE(reconcile_failed.load()) << "round " << round;

    if (link_won.load()) {
      metadata::FileChunkSnapshot attached;
      ASSERT_TRUE(meta_->ReadFileChunkSnapshot(ino, 0, &attached).ok());
      EXPECT_TRUE(attached.chunk_id.has_value());
      EXPECT_TRUE(data_->Contains(key));
      ASSERT_TRUE(meta_->Unlink(kRootInodeId, "revived-race").ok());
      ASSERT_TRUE(ReconcileAll(data_, typed_metadata_).ok());
    } else {
      EXPECT_TRUE(ReadInodeStatus(ino).IsNotFound());
      EXPECT_FALSE(data_->Contains(key));
    }
  }
}

FIBER_TEST_F(ReclaimerTest, TypedRevisionCleanupDoesNotKeepStaleCachedReadSnapshot) {
  constexpr InodeID kIno = 42;
  auto typed = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID id;
  ASSERT_TRUE(typed->AllocateChunkID(&id).ok());
  metadata::cow::COWChunkRevision first;
  ASSERT_TRUE(typed->AllocateRevision(id, &first).ok());
  const metadata::cow::COWChunkHead initial{first, 64};
  ASSERT_TRUE(typed->CompareExchangeHead(id, std::nullopt, initial).ok());
  const chunk::cow::COWObjectKey old_key(id, first);
  data_->Seed(std::string(static_cast<std::string_view>(old_key)));

  auto cached_reader = std::make_unique<chunk::cow::COWChunk>(kIno, 0, 1024, typed, meta_, data_, id, initial, 64);
  metadata::cow::COWChunkRevision second;
  ASSERT_TRUE(typed->AllocateRevision(id, &second).ok());
  ASSERT_TRUE(typed->CompareExchangeHead(id, initial, {second, 64}).ok());
  const chunk::cow::COWObjectKey new_key(id, second);
  ASSERT_TRUE(data_->Put(static_cast<std::string_view>(new_key), folly::IOBuf::copyBuffer(std::string(64, 'N'))).ok());

  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(1024, typed, meta_, data_, {}, &cleanup).ok());
  const auto pending = MakeTypedRevisionDelete(id, first);
  bool completed = true;
  ASSERT_TRUE(cleanup->DeletePending(pending, &completed).ok());
  EXPECT_TRUE(completed) << "a cached clean session must not keep an obsolete immutable revision alive";
  EXPECT_FALSE(data_->Contains(static_cast<std::string_view>(old_key)));
  auto out = folly::IOBuf::create(3);
  ASSERT_TRUE(cached_reader->Read(0, 3, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "NNN");
}

TEST_F(ReclaimerTest, TypedRevisionReadRetriesNewHeadIfGcDeletedPreviousObjectDuringGet) {
  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_started;
  folly::fibers::Baton release_get;
  folly::fibers::Baton read_done;
  folly::fibers::Baton finished;
  constexpr InodeID kIno = 42;
  auto typed = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  metadata::ChunkID id;
  metadata::cow::COWChunkRevision old_revision;
  metadata::cow::COWChunkRevision next_revision;
  std::unique_ptr<chunk::cow::COWChunk> reader;
  std::unique_ptr<chunk::internal::ChunkCleanupParticipant> cleanup;
  Status read_status;

  manager.addTask([&] {
    ASSERT_TRUE(typed->AllocateChunkID(&id).ok());
    ASSERT_TRUE(typed->AllocateRevision(id, &old_revision).ok());
    const metadata::cow::COWChunkHead original{old_revision, 6};
    ASSERT_TRUE(typed->CompareExchangeHead(id, std::nullopt, original).ok());
    const chunk::cow::COWObjectKey key(id, old_revision);
    ASSERT_TRUE(data_->Put(static_cast<std::string_view>(key), folly::IOBuf::copyBuffer("seeded")).ok());
    reader = std::make_unique<chunk::cow::COWChunk>(kIno, 0, 1024, typed, meta_, data_, id, original, 6);
    ASSERT_TRUE(typed->AllocateRevision(id, &next_revision).ok());
    const chunk::cow::COWObjectKey new_key(id, next_revision);
    ASSERT_TRUE(data_->Put(static_cast<std::string_view>(new_key), folly::IOBuf::copyBuffer("newest")).ok());
    ASSERT_TRUE(chunk::cow::CreateCOWCleanupParticipant(1024, typed, meta_, data_, {}, &cleanup).ok());
    data_->BlockNextGet(&get_started, &release_get);
    auto out = folly::IOBuf::create(6);
    read_status = reader->Read(0, 6, out.get());
    if (read_status.ok()) {
      EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "newest");
    }
    read_done.post();
  });
  manager.addTask([&] {
    get_started.wait();
    const metadata::cow::COWChunkHead expected{old_revision, 6};
    ASSERT_TRUE(typed->CompareExchangeHead(id, expected, {next_revision, 6}).ok());
    bool completed = true;
    const auto work = MakeTypedRevisionDelete(id, old_revision);
    ASSERT_TRUE(cleanup->DeletePending(work, &completed).ok());
    EXPECT_TRUE(completed) << "GC can retire old revision without holding a cached read snapshot";
    release_get.post();
    read_done.wait();
    EXPECT_TRUE(read_status.ok()) << read_status.message();
    finished.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return finished.try_wait(); }, "typed COW retired revision read retry");
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

// ────────────────────────────────────────────────────────────────
// #141: a reclaim must never delete objects a Link can still revive
// ────────────────────────────────────────────────────────────────
// The link and the reclaim race on the same inode. Exactly one outcome is
// legal: the link revived the inode — and then its chunk metadata and its
// object must both still be there — or the reclaim prepared first — and then
// the link must have failed and the object must be gone. A revived inode
// whose object was deleted would be silent data loss.

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
    config.chunk_type = metadata::ChunkType::kCow;
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
    config.chunk_type = metadata::ChunkType::kCow;
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
    config.chunk_type = metadata::ChunkType::kCow;
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
    config.chunk_type = metadata::ChunkType::kCow;
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
