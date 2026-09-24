// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for the reclaim driver: the cross-engine sequence that turns an
// inode's orphan candidate (published by unlink/rename-overwrite) into a
// completed data cleanup, and the reconciliation that recovers crash-left
// work. The metadata engine under test is the real memory backend, and the
// data engine is a recording fake, so the frozen object identities the
// metadata engine produces are the ones the driver is observed to delete.

#include <fcntl.h>
#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/IChunkOverwriteStrategy.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/types/Reclaim.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/Reclaimer.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsVolume;
using swordfs::utils::Status;

metadata::PendingDelete MakePendingDelete(InodeID ino, const SwordFsChunk &descriptor) {
  metadata::PendingDelete pending;
  EXPECT_TRUE(chunk::FreezeWholeObjectDelete(ino, descriptor, 0, &pending).ok());
  return pending;
}

// Data engine that records every Delete and can fail selected keys.
class RecordingDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    objects_[std::string(key)] = std::string(reinterpret_cast<const char *>(data->data()), data->length());
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view key) override {
    const std::string owned(key);
    delete_calls.push_back(owned);
    auto it = fail_keys.find(owned);
    if (it != fail_keys.end()) {
      return it->second;
    }
    objects_.erase(owned);
    return Status::OK();
  }

  void Seed(std::string key) {
    objects_[std::move(key)] = "seeded";
  }
  bool Contains(std::string_view key) const {
    return objects_.find(std::string(key)) != objects_.end();
  }

  std::vector<std::string> delete_calls;
  std::unordered_map<std::string, Status> fail_keys;

 private:
  std::unordered_map<std::string, std::string> objects_;
};

class StagedIntentMetaEngine : public MemMetaImpl {
 public:
  explicit StagedIntentMetaEngine(metadata::PendingDelete pending) : pending_(std::move(pending)) {
    chunk::WholeObjectRef ref;
    const auto status = chunk::DecodeWholeObjectDelete(*pending_, 0, &ref);
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

class PartialReclaimMetaEngine : public MemMetaImpl {
 public:
  explicit PartialReclaimMetaEngine(metadata::ReclaimWork frozen) : frozen_(std::move(frozen)) {
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
  Status PrepareReclaim(InodeID file_ino, std::optional<metadata::ReclaimWork> *work) override {
    if (file_ino != frozen_.ino) {
      return Status::NotFound("reclaim inode mismatch");
    }
    ++prepare_calls;
    live_ = false;
    *work = frozen_;
    return Status::OK();
  }
  Status GetInode(InodeID file_ino, SwordFsInode *out) override {
    if (!live_ || file_ino != frozen_.ino) {
      return Status::NotFound("inode already removed");
    }
    if (out != nullptr) {
      metadata::SwordFsAttr attr(file_ino, S_IFREG | 0644);
      attr.nlink = 0;
      *out = SwordFsInode(file_ino, attr, kRootInodeId);
    }
    return Status::OK();
  }
  Status CompleteReclaim(InodeID file_ino) override {
    if (file_ino != frozen_.ino) {
      return Status::NotFound("reclaim inode mismatch");
    }
    completed = true;
    return Status::OK();
  }

  int prepare_calls = 0;
  bool completed = false;

 private:
  metadata::ReclaimWork frozen_;
  bool live_ = true;
};

class ReclaimerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<MemMetaImpl>>();
    auto data = std::make_unique<RecordingDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    SwordFsVolume config;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    // A test that started the reclaim worker must never let it outlive the
    // engines it walks; Stop() is idempotent and safe when never started.
    Reclaimer::Instance().Stop();
    // Drop per-inode runtime state and the injected engines on the threads
    // that own them.
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  // Create a regular file with one published chunk, and seed the matching
  // object in the data engine. Returns the inode id.
  InodeID CreateChunkedFile(std::string_view name, uint64_t revision = 1) {
    SwordFsInode file;
    auto status = meta_->Create(kRootInodeId, name, 0644, &file);
    EXPECT_TRUE(status.ok()) << status.message();
    SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = revision, .size = 64};
    status = meta_->CommitChunk(file.ino, std::nullopt, chunk);
    EXPECT_TRUE(status.ok()) << status.message();
    data_->Seed(chunk::FormatChunkObjectKey(file.ino, 0, revision));
    return file.ino;
  }

  std::vector<InodeID> OrphanCandidates() {
    std::vector<InodeID> out;
    auto status = meta_->VisitOrphanCandidates([&out](InodeID ino) {
      out.push_back(ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    return out;
  }

  std::vector<InodeID> PendingReclaims() {
    std::vector<InodeID> out;
    auto status = meta_->VisitPendingReclaims([&out](const metadata::ReclaimWork &work) {
      out.push_back(work.ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    return out;
  }

  std::vector<std::string> PendingDeletes() {
    std::vector<std::string> out;
    bool has_more = false;
    auto status = meta_->VisitPendingDeletesBatch(
        1024,
        [&out](const metadata::PendingDelete &work) {
          chunk::WholeObjectRef ref;
          auto status = chunk::DecodeWholeObjectDelete(work, volume::VolumeImpl::Instance().chunk_size(), &ref);
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

  MemMetaImpl *meta_ = nullptr;
  RecordingDataEngine *data_ = nullptr;
};

FIBER_TEST_F(ReclaimerTest, ReconcileDeletesFrozenObjectsAndCompletes) {
  const InodeID f_ino = CreateChunkedFile("f");
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());

  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  EXPECT_EQ(data_->delete_calls, std::vector<std::string>{key});
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound());
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_TRUE(OrphanCandidates().empty());

  // Nothing left to do: a second pass is a no-op.
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_EQ(data_->delete_calls.size(), 1U);

  // The prepared output is optional; omitting it must not change the reclaim
  // sequence or leave the frozen record behind.
  const InodeID second = CreateChunkedFile("second", 2);
  const auto second_key = chunk::FormatChunkObjectKey(second, 0, 2);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "second").ok());
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(second_key));
  EXPECT_TRUE(meta_->GetInode(second, nullptr).IsNotFound());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(ReclaimerTest, FrozenWorkDoesNotAuthorizeDeletingALiveInode) {
  const InodeID file_ino = CreateChunkedFile("partially-prepared");
  const auto key = chunk::FormatChunkObjectKey(file_ino, 0, 1);
  SwordFsChunk head;
  ASSERT_TRUE(meta_->FindChunk(file_ino, 0, &head).ok());

  metadata::ReclaimWork frozen;
  ASSERT_TRUE(chunk::FreezeWholeObjectReclaim(file_ino, {head}, 0, &frozen).ok());
  const auto *strategy = volume::VolumeImpl::Instance().chunk_overwrite_strategy();
  const auto status = strategy->DeleteFrozen(frozen, 0, meta_, data_);
  EXPECT_TRUE(status.ToErrno() == EBUSY) << status.message();
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->delete_calls.empty());

  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "partially-prepared").ok());
  std::optional<metadata::ReclaimWork> prepared;
  ASSERT_TRUE(meta_->PrepareReclaim(file_ino, &prepared).ok());
  ASSERT_TRUE(prepared.has_value());
  ASSERT_TRUE(strategy->DeleteFrozen(*prepared, 0, meta_, data_).ok());
  EXPECT_FALSE(data_->Contains(key));
  ASSERT_TRUE(meta_->CompleteReclaim(file_ino).ok());
}

TEST_F(ReclaimerTest, PendingReclaimReplaysPreparationBeforeDeletion) {
  constexpr InodeID kFileIno = 42;
  const SwordFsChunk head{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  metadata::ReclaimWork frozen;
  ASSERT_TRUE(chunk::FreezeWholeObjectReclaim(kFileIno, {head}, 0, &frozen).ok());
  auto replacement = std::make_unique<swordfs::test::ConfiguredMetaEngine<PartialReclaimMetaEngine>>(frozen);
  auto *partial_meta = replacement.get();
  auto data = std::make_unique<RecordingDataEngine>();
  data_ = data.get();
  SwordFsVolume config;
  const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(replacement), std::move(data), std::move(config));
  ASSERT_TRUE(status.ok()) << status.message();
  const auto key = chunk::FormatChunkObjectKey(kFileIno, 0, 7);
  data_->Seed(key);

  swordfs::test::RunInTestFiber([&] { ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok()); });
  EXPECT_EQ(partial_meta->prepare_calls, 1);
  EXPECT_TRUE(partial_meta->completed);
  EXPECT_FALSE(data_->Contains(key));
}

FIBER_TEST_F(ReclaimerTest, ReconcileRetriesFailedObjectDeletes) {
  const InodeID f_ino = CreateChunkedFile("f");
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  data_->fail_keys[key] = Status::IOError("injected delete failure");

  const auto first = Reclaimer::Instance().Reconcile();
  EXPECT_FALSE(first.ok());
  EXPECT_EQ(data_->delete_calls.size(), 1U);
  // The frozen record survives the failed delete...
  EXPECT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  // ... and reconciliation completes it (idempotently) once the backend
  // recovers.
  data_->fail_keys.clear();
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_EQ(data_->delete_calls.size(), 2U);
  EXPECT_EQ(data_->delete_calls[1], key);
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound());
}

FIBER_TEST_F(ReclaimerTest, TruncateCleanupDoesNotDependOnLocalChunkCache) {
  const InodeID f_ino = CreateChunkedFile("truncate");
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);

  // Call the authoritative metadata engine directly: this deliberately skips
  // FileReadWriter/FileChunkManager, modelling a cold cache or a fresh mount.
  ASSERT_TRUE(meta_->Truncate(f_ino, 0).ok());
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{key});
  EXPECT_TRUE(data_->Contains(key));

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(PendingDeletes().empty());
}

FIBER_TEST_F(ReclaimerTest, TruncateCleanupRetriesFailedDelete) {
  const InodeID f_ino = CreateChunkedFile("truncate-retry");
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Truncate(f_ino, 0).ok());
  data_->fail_keys[key] = Status::IOError("injected delete failure");

  EXPECT_EQ(Reclaimer::Instance().Reconcile().ToErrno(), EIO);
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{key});
  EXPECT_TRUE(data_->Contains(key));

  data_->fail_keys.clear();
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(PendingDeletes().empty());
  EXPECT_FALSE(data_->Contains(key));
}

FIBER_TEST_F(ReclaimerTest, RewriteCleanupDeletesOnlySupersededRevision) {
  const InodeID f_ino = CreateChunkedFile("rewrite");
  SwordFsChunk first;
  ASSERT_TRUE(meta_->FindChunk(f_ino, 0, &first).ok());

  auto replacement = first;
  replacement.revision = first.revision + 1;
  const auto old_key = chunk::FormatChunkObjectKey(f_ino, first.index, first.revision);
  const auto new_key = chunk::FormatChunkObjectKey(f_ino, replacement.index, replacement.revision);
  data_->Seed(new_key);

  ASSERT_TRUE(meta_->CommitChunk(f_ino, first, replacement).ok());
  EXPECT_EQ(PendingDeletes(), std::vector<std::string>{old_key});
  EXPECT_TRUE(data_->Contains(old_key));
  EXPECT_TRUE(data_->Contains(new_key));

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(old_key));
  EXPECT_TRUE(data_->Contains(new_key));
  EXPECT_TRUE(PendingDeletes().empty());

  SwordFsChunk authoritative;
  ASSERT_TRUE(meta_->FindChunk(f_ino, 0, &authoritative).ok());
  EXPECT_EQ(authoritative, replacement);
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteCandidateDoesNotDeleteStillAuthoritativeObject) {
  constexpr InodeID kIno = 42;
  SwordFsChunk descriptor{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  const auto key = chunk::FormatChunkObjectKey(kIno, descriptor.index, descriptor.revision);
  metadata::PendingDelete pending = MakePendingDelete(kIno, descriptor);

  StagedIntentMetaEngine *staged = nullptr;
  // Metadata-engine construction/destruction is control-plane work. Replace
  // the fixture's MemMetaImpl on a POSIX thread so the Debug execution-domain
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
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(staged->completed_keys.empty());

  // Once authoritative metadata no longer names that immutable object, the
  // same candidate becomes executable and is acknowledged only after deletion.
  staged->SetCurrent(std::nullopt);
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_EQ(data_->delete_calls, std::vector<std::string>{key});
  EXPECT_EQ(staged->completed_keys, std::vector<std::string>{pending.id});
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteFailsClosedWhenAuthoritativeChunkLookupFails) {
  constexpr InodeID kIno = 42;
  SwordFsChunk descriptor{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  const auto key = chunk::FormatChunkObjectKey(kIno, descriptor.index, descriptor.revision);
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

  const auto status = Reclaimer::Instance().Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(staged->completed_keys.empty());
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteRemovesSupersededRevisionWhileNewRevisionStaysAuthoritative) {
  constexpr InodeID kIno = 42;
  SwordFsChunk old_descriptor{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  const auto old_key = chunk::FormatChunkObjectKey(kIno, old_descriptor.index, old_descriptor.revision);
  metadata::PendingDelete pending = MakePendingDelete(kIno, old_descriptor);

  StagedIntentMetaEngine *staged = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<StagedIntentMetaEngine>>(pending);
    staged = staged_meta.get();
    staged->SetCurrent(SwordFsChunk{.index = 0, .start_offset = 0, .revision = 8, .size = 64});
    auto data = std::make_unique<RecordingDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status =
        swordfs::test::LoadTestVolumeRuntime(std::move(staged_meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  data_->Seed(old_key);

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(old_key));
  EXPECT_EQ(data_->delete_calls, std::vector<std::string>{old_key});
  EXPECT_EQ(staged->completed_keys, std::vector<std::string>{pending.id});
}

FIBER_TEST_F(ReclaimerTest, ReconcileRecoversCrashLeftOrphan) {
  // Crash model: the unlink published the orphan candidate and the process
  // died before any reclaim ran. A fresh mount has no descriptors and no
  // handle state, so mount-time reconciliation is exactly what runs here.
  const InodeID f_ino = CreateChunkedFile("f");
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  ASSERT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});

  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  EXPECT_TRUE(data_->Contains(key)) << "nothing may be deleted before the reclaim is prepared";

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());

  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(ReclaimerTest, ReconcileLeavesRevivedInodeAlone) {
  const InodeID f_ino = CreateChunkedFile("f");
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());

  // A hard link revives the inode before reconciliation runs (the marker is
  // dropped by the link transaction itself).
  SwordFsInode revived;
  ASSERT_TRUE(meta_->Link(f_ino, kRootInodeId, "revived", &revived).ok());
  EXPECT_TRUE(OrphanCandidates().empty());

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());

  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  EXPECT_TRUE(data_->delete_calls.empty()) << "no object may be deleted while a name references the inode";
  EXPECT_TRUE(data_->Contains(key));
  ASSERT_TRUE(meta_->GetInode(f_ino, &revived).ok());
  EXPECT_EQ(revived.attr.nlink, 1U);
}

FIBER_TEST_F(ReclaimerTest, ReconcileDefersUntilAfterTheLastDescriptorCloses) {
  const InodeID f_ino = CreateChunkedFile("f");

  // Open a descriptor, then unlink: the reclaim must be deferred while the
  // descriptor lives, and the inode must survive for it.
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(f_ino, O_RDONLY, &handle).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(data_->delete_calls.empty()) << "an open descriptor must defer the cleanup";
  ASSERT_TRUE(meta_->GetInode(f_ino, nullptr).ok());

  // Close only releases the local reference; background reclaim remains the
  // sole executor and completes on the next pass.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound());

  // Once reclaimed, the inode cannot be reopened.
  std::shared_ptr<FileHandle> reopened;
  EXPECT_TRUE(FileHandle::Open(f_ino, O_RDONLY, &reopened).IsNotFound());
}

FIBER_TEST_F(ReclaimerTest, ReconcileDefersAnUnlinkedInodeWithAnOpenDescriptor) {
  // #143: the background reconciliation is a reclaim caller like any other, so
  // it must go through the same per-inode decision. While a descriptor holds
  // the unlinked inode open, a full Reconcile pass may not delete the inode or
  // its objects.
  const InodeID f_ino = CreateChunkedFile("f");
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(f_ino, O_RDONLY, &handle).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());
  ASSERT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());

  // Nothing was reclaimed: the candidate is still durable, the inode still
  // resolves, and the object the open descriptor can still read is untouched.
  EXPECT_TRUE(data_->delete_calls.empty()) << "reconciliation must not reclaim an inode a descriptor still holds";
  EXPECT_TRUE(data_->Contains(key));
  ASSERT_TRUE(meta_->GetInode(f_ino, nullptr).ok()) << "the inode must survive while the descriptor is open";
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{f_ino}));
  EXPECT_TRUE(PendingReclaims().empty());

  // Last close only releases the local reference. The next worker pass owns
  // preparation/deletion/completion.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_EQ(data_->delete_calls, (std::vector<std::string>{key}));
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound());
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

FIBER_TEST_F(ReclaimerTest, ConcurrentReclaimAndLinkNeverDeleteLiveData) {
  constexpr int kRounds = 100;

  std::atomic<int> revived{0};
  std::atomic<int> reclaimed{0};
  std::atomic<int> failures{0};

  for (int round = 0; round < kRounds; ++round) {
    const InodeID f_ino = CreateChunkedFile("race");
    const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
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
      if (!Reclaimer::Instance().Reconcile().ok()) {
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
      ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
    } else {
      // The reclaim won: the inode is gone and so is its object.
      EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound()) << "round " << round;
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
    Reclaimer::Instance().Stop();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }
};

FIBER_TEST_F(ReclaimerNoEngineTest, ReconcileWithoutEnginesIsANoOp) {
  EXPECT_TRUE(Reclaimer::Instance().Reconcile().ok());
}

FIBER_TEST_F(ReclaimerNoEngineTest, MissingDataEngineFailsClosedAndReconcileStaysHarmless) {
  swordfs::test::RunInTestThreadFromFiber([&] {
    SwordFsVolume config;
    auto configured = std::make_unique<swordfs::test::ConfiguredMetaEngine<MemMetaImpl>>();
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(configured), nullptr, std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  ASSERT_NE(volume::VolumeImpl::Instance().meta_engine(), nullptr);
  ASSERT_EQ(volume::VolumeImpl::Instance().data_engine(), nullptr);

  EXPECT_TRUE(Reclaimer::Instance().Reconcile().ok());
}

// ────────────────────────────────────────────────────────────────
// #143/#142: one failing cleanup item must not hide the rest of the work
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(ReclaimerTest, ReconcileCountsEveryFailedCleanupItem) {
  // One truncate pending-delete, one orphan candidate and one already-frozen
  // pending reclaim all fail. The pass must attempt all three and report the
  // aggregate — a failure in one durable queue must not hide another.
  const InodeID truncated = CreateChunkedFile("truncated");
  const InodeID orphan = CreateChunkedFile("orphan");
  const InodeID frozen = CreateChunkedFile("frozen");
  const auto truncated_key = chunk::FormatChunkObjectKey(truncated, 0, 1);
  const auto orphan_key = chunk::FormatChunkObjectKey(orphan, 0, 1);
  const auto frozen_key = chunk::FormatChunkObjectKey(frozen, 0, 1);
  ASSERT_TRUE(meta_->Truncate(truncated, 0).ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "orphan").ok());
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "frozen").ok());

  // Freeze one directly through metadata to model a crash-left pending record
  // without involving the worker path under test.
  std::optional<metadata::ReclaimWork> frozen_work;
  ASSERT_TRUE(meta_->PrepareReclaim(frozen, &frozen_work).ok());
  ASSERT_TRUE(frozen_work.has_value());
  data_->fail_keys[frozen_key] = Status::IOError("injected delete failure");
  ASSERT_EQ(PendingReclaims(), std::vector<InodeID>{frozen});

  data_->fail_keys[truncated_key] = Status::IOError("injected delete failure");
  data_->fail_keys[orphan_key] = Status::IOError("injected delete failure");
  const auto status = Reclaimer::Instance().Reconcile();
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
  data_->fail_keys.clear();
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(PendingDeletes().empty());
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_FALSE(data_->Contains(truncated_key));
  EXPECT_FALSE(data_->Contains(orphan_key));
  EXPECT_FALSE(data_->Contains(frozen_key));
  EXPECT_TRUE(meta_->GetInode(truncated, nullptr).ok());
  EXPECT_TRUE(meta_->GetInode(orphan, nullptr).IsNotFound());
  EXPECT_TRUE(meta_->GetInode(frozen, nullptr).IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// #143: a backend that cannot answer the scan must be reported
// ────────────────────────────────────────────────────────────────
// Reconcile() walks two durable sets; a backend failure on either scan must
// reach the caller instead of being read as an empty set.

class FailingScanMetaEngine : public MemMetaImpl {
 public:
  Status VisitPendingDeletesBatch(size_t max_items, const swordfs::metadata::PendingDeleteVisitorFn &visitor,
                                  bool *has_more) override {
    if (!pending_delete_scan_status.ok()) {
      return pending_delete_scan_status;
    }
    return MemMetaImpl::VisitPendingDeletesBatch(max_items, visitor, has_more);
  }

  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &visitor) override {
    if (!orphan_scan_status.ok()) {
      return orphan_scan_status;
    }
    return MemMetaImpl::VisitOrphanCandidates(visitor);
  }

  Status VisitPendingReclaims(const swordfs::metadata::ReclaimVisitorFn &visitor) override {
    if (!pending_scan_status.ok()) {
      return pending_scan_status;
    }
    return MemMetaImpl::VisitPendingReclaims(visitor);
  }

  Status orphan_scan_status = Status::OK();
  Status pending_scan_status = Status::OK();
  Status pending_delete_scan_status = Status::OK();
};

class ReclaimerScanFailureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<FailingScanMetaEngine>>();
    meta_ = meta.get();
    SwordFsVolume config;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::make_unique<RecordingDataEngine>(),
                                                             std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  FailingScanMetaEngine *meta_ = nullptr;
};

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesOrphanScanFailure) {
  meta_->orphan_scan_status = Status::IOError("orphan scan unavailable");

  const auto status = Reclaimer::Instance().Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(status.message(), "orphan scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesPendingDeleteScanFailure) {
  meta_->pending_delete_scan_status = Status::IOError("pending delete scan unavailable");

  const auto status = Reclaimer::Instance().Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(status.message(), "pending delete scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesPendingScanFailure) {
  // The orphan scan is healthy; the second visitor's failure must reach the
  // caller just the same.
  meta_->pending_scan_status = Status::IOError("pending scan unavailable");

  const auto status = Reclaimer::Instance().Reconcile();
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

FIBER_TEST_F(ReclaimerTest, WorkerCompletesAPendingReclaimImmediatelyAtStartup) {
  const InodeID f_ino = CreateChunkedFile("f");
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "f").ok());

  std::optional<metadata::ReclaimWork> frozen_work;
  ASSERT_TRUE(meta_->PrepareReclaim(f_ino, &frozen_work).ok());
  ASSERT_TRUE(frozen_work.has_value());
  ASSERT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  swordfs::test::RunInTestThreadFromFiber([&] { Reclaimer::Instance().Start(); });

  // Start() runs the first pass immediately; give the worker room to hand the
  // pass to its fiber runtime and complete it.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !PendingReclaims().empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const bool completed = PendingReclaims().empty();

  swordfs::test::RunInTestThreadFromFiber([&] { Reclaimer::Instance().Stop(); });

  EXPECT_TRUE(completed) << "the startup worker pass must finish crash-left reclaim work";
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(f_ino, nullptr).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
}

FIBER_TEST_F(ReclaimerTest, WorkerSelfWakesUntilLargePendingDeleteBacklogIsDrained) {
  constexpr size_t kBatchSize = 128;
  constexpr size_t kChunkCount = kBatchSize + 2;
  const uint64_t chunk_size = metadata::SwordFsVolume{}.chunk_size;
  SwordFsInode file;
  ASSERT_TRUE(meta_->Create(kRootInodeId, "pending-delete-backlog", 0644, &file).ok());
  for (size_t i = 0; i < kChunkCount; ++i) {
    SwordFsChunk chunk{.index = static_cast<metadata::ChunkIndex>(i),
                       .start_offset = static_cast<uint64_t>(i) * chunk_size,
                       .revision = i + 1,
                       .size = 64};
    ASSERT_TRUE(meta_->CommitChunk(file.ino, std::nullopt, chunk).ok());
    data_->Seed(chunk::FormatChunkObjectKey(file.ino, chunk.index, chunk.revision));
  }
  ASSERT_TRUE(meta_->Truncate(file.ino, 0).ok());
  ASSERT_EQ(PendingDeletes().size(), kChunkCount);

  const InodeID orphan_ino = CreateChunkedFile("backlog-orphan", 1000);
  const auto orphan_key = chunk::FormatChunkObjectKey(orphan_ino, 0, 1000);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "backlog-orphan").ok());

  swordfs::test::RunInTestThreadFromFiber([&] { Reclaimer::Instance().Start(); });

  // The safety scan is five seconds. Finishing strictly before that interval
  // proves the second pending-delete batch came from has_more -> Wake(), not
  // from the periodic fallback.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < deadline &&
         (!PendingDeletes().empty() || !meta_->GetInode(orphan_ino, nullptr).IsNotFound())) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const bool completed = PendingDeletes().empty() && meta_->GetInode(orphan_ino, nullptr).IsNotFound();

  swordfs::test::RunInTestThreadFromFiber([&] { Reclaimer::Instance().Stop(); });

  EXPECT_TRUE(completed) << "worker must self-wake for the second bounded pending-delete pass";
  ASSERT_EQ(data_->delete_calls.size(), kChunkCount + 1);
  EXPECT_EQ(data_->delete_calls[kBatchSize], orphan_key)
      << "orphan work must run after the first 128-item pending-delete batch and before its continuation";
}

FIBER_TEST_F(ReclaimerTest, WorkerSurvivesAFailedPassAndRecoversOnWake) {
  const InodeID f_ino = CreateChunkedFile("retry-failure");
  const auto key = chunk::FormatChunkObjectKey(f_ino, 0, 1);
  ASSERT_TRUE(meta_->Unlink(kRootInodeId, "retry-failure").ok());

  // Freeze the reclaim first, and keep the backend failing so the first worker
  // pass reports an error instead of completing the record.
  std::optional<metadata::ReclaimWork> frozen_work;
  ASSERT_TRUE(meta_->PrepareReclaim(f_ino, &frozen_work).ok());
  ASSERT_TRUE(frozen_work.has_value());
  data_->fail_keys[key] = Status::IOError("injected persistent delete failure");
  ASSERT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});
  const auto initial_delete_calls = data_->delete_calls.size();

  swordfs::test::RunInTestThreadFromFiber([&] { Reclaimer::Instance().Start(); });

  const auto failed_pass_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < failed_pass_deadline &&
         data_->delete_calls.size() == initial_delete_calls) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const bool failed_pass_ran = data_->delete_calls.size() > initial_delete_calls;
  EXPECT_TRUE(failed_pass_ran) << "the first worker pass must retry the still-failing delete";
  EXPECT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});

  // Recover the backend and explicitly wake the same worker; the failed pass
  // must not terminate it and wakeup must not wait for the safety interval.
  data_->fail_keys.clear();
  Reclaimer::Instance().Wake();
  const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < recovery_deadline && !PendingReclaims().empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const bool recovered = PendingReclaims().empty();

  swordfs::test::RunInTestThreadFromFiber([&] { Reclaimer::Instance().Stop(); });

  EXPECT_TRUE(recovered) << "a failed worker pass must not terminate the reclaim worker";
  EXPECT_FALSE(data_->Contains(key));
}

FIBER_TEST_F(ReclaimerTest, WorkerLifecycleIsIdempotent) {
  // Stopping before any start is a no-op, a second start must not leave a
  // second thread behind, and a stop must return promptly instead of waiting
  // out the retry interval.
  const auto started = std::chrono::steady_clock::now();
  swordfs::test::RunInTestThreadFromFiber([&] {
    Reclaimer::Instance().Stop();  // no thread was ever started
    Reclaimer::Instance().Start();
    Reclaimer::Instance().Start();  // idempotent
    Reclaimer::Instance().Stop();
    Reclaimer::Instance().Stop();  // already joined
  });
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // Stop posts the cross-domain waiter, so teardown should not pay either the
  // five-second retry interval or a polling-slice delay. Keep a generous CI
  // bound while still pinning the prompt-shutdown contract.
  EXPECT_LT(elapsed, std::chrono::milliseconds(500)) << "Stop must wake the reclaim worker promptly";
}

}  // namespace
}  // namespace swordfs::vfs
