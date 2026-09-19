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
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
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
using swordfs::utils::Status;

// Data engine that records every Delete and can fail selected keys.
class RecordingDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  swordfs::storage::DataEngineLimits Limits() const override {
    return {};
  }
  bool Head(std::string_view key, size_t *size) override {
    auto it = objects_.find(std::string(key));
    if (it == objects_.end()) {
      return false;
    }
    if (size != nullptr) {
      *size = it->second.size();
    }
    return true;
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
  explicit StagedIntentMetaEngine(metadata::PendingDelete pending)
      : pending_ino_(pending.ino), pending_(std::move(pending)) {
    current_ = pending_->chunk.descriptor;
  }

  Status VisitPendingDeletes(const metadata::PendingDeleteVisitorFn &visitor) override {
    if (!pending_.has_value()) {
      return Status::OK();
    }
    return visitor(*pending_);
  }

  Status CompletePendingDelete(std::string_view key) override {
    if (!pending_.has_value() || key != pending_->chunk.key) {
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

class ReclaimerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Volume lifecycle is control-plane work and follows the production
    // topology: initialize it on the gtest POSIX thread, then reset the
    // fiber-owned handle registry from a fiber.
    volume::VolumeImpl::Initialize();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });

    auto meta = std::make_unique<MemMetaImpl>();
    auto data = std::make_unique<RecordingDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    auto &vol = volume::VolumeImpl::Instance();
    vol.set_meta_engine(std::move(meta));
    vol.set_data_engine(std::move(data));
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
    auto status = meta_->VisitPendingDeletes([&out](const metadata::PendingDelete &work) {
      out.push_back(work.chunk.key);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
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

  EXPECT_EQ(Reclaimer::Instance().Reconcile().code(), Status::kIOError);
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

FIBER_TEST_F(ReclaimerTest, PreparedTruncateIntentDoesNotDeleteStillAuthoritativeObject) {
  constexpr InodeID kIno = 42;
  SwordFsChunk descriptor{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  const auto key = chunk::FormatChunkObjectKey(kIno, descriptor.index, descriptor.revision);
  metadata::PendingDelete pending{.ino = kIno, .chunk = metadata::ReclaimChunk{.descriptor = descriptor, .key = key}};

  StagedIntentMetaEngine *staged = nullptr;
  // Metadata-engine construction/destruction is control-plane work. Replace
  // the fixture's MemMetaImpl on a POSIX thread so the Debug execution-domain
  // contract remains identical to production lifecycle.
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<StagedIntentMetaEngine>(pending);
    staged = staged_meta.get();
    volume::VolumeImpl::Instance().set_meta_engine(std::move(staged_meta));
  });
  data_->Seed(key);

  // Preparation has made the intent durable, but the chunk descriptor is
  // still authoritative. Reconciliation must leave both object and intent
  // untouched rather than racing the later detach transaction.
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(staged->completed_keys.empty());

  // Once authoritative metadata no longer names that immutable object, the
  // same intent becomes executable and is acknowledged only after deletion.
  staged->SetCurrent(std::nullopt);
  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_EQ(data_->delete_calls, std::vector<std::string>{key});
  EXPECT_EQ(staged->completed_keys, std::vector<std::string>{key});
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteFailsClosedWhenAuthoritativeChunkLookupFails) {
  constexpr InodeID kIno = 42;
  SwordFsChunk descriptor{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  const auto key = chunk::FormatChunkObjectKey(kIno, descriptor.index, descriptor.revision);
  metadata::PendingDelete pending{.ino = kIno, .chunk = metadata::ReclaimChunk{.descriptor = descriptor, .key = key}};

  StagedIntentMetaEngine *staged = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<StagedIntentMetaEngine>(pending);
    staged = staged_meta.get();
    staged->SetFindStatus(Status::IOError("chunk lookup unavailable"));
    volume::VolumeImpl::Instance().set_meta_engine(std::move(staged_meta));
  });
  data_->Seed(key);

  const auto status = Reclaimer::Instance().Reconcile();
  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(staged->completed_keys.empty());
}

FIBER_TEST_F(ReclaimerTest, PendingDeleteRemovesSupersededRevisionWhileNewRevisionStaysAuthoritative) {
  constexpr InodeID kIno = 42;
  SwordFsChunk old_descriptor{.index = 0, .start_offset = 0, .revision = 7, .size = 64};
  const auto old_key = chunk::FormatChunkObjectKey(kIno, old_descriptor.index, old_descriptor.revision);
  metadata::PendingDelete pending{.ino = kIno,
                                  .chunk = metadata::ReclaimChunk{.descriptor = old_descriptor, .key = old_key}};

  StagedIntentMetaEngine *staged = nullptr;
  swordfs::test::RunInTestThreadFromFiber([&] {
    auto staged_meta = std::make_unique<StagedIntentMetaEngine>(pending);
    staged = staged_meta.get();
    staged->SetCurrent(SwordFsChunk{.index = 0, .start_offset = 0, .revision = 8, .size = 64});
    volume::VolumeImpl::Instance().set_meta_engine(std::move(staged_meta));
  });
  data_->Seed(old_key);

  ASSERT_TRUE(Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(old_key));
  EXPECT_EQ(data_->delete_calls, std::vector<std::string>{old_key});
  EXPECT_EQ(staged->completed_keys, std::vector<std::string>{old_key});
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
  std::unique_ptr<MemMetaImpl> meta;
  swordfs::test::RunInTestThreadFromFiber([&] { meta = std::make_unique<MemMetaImpl>(); });
  volume::VolumeImpl::Instance().set_meta_engine(std::move(meta));
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
  EXPECT_EQ(status.code(), Status::kIOError) << status.message();
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
  Status VisitPendingDeletes(const swordfs::metadata::PendingDeleteVisitorFn &visitor) override {
    if (!pending_delete_scan_status.ok()) {
      return pending_delete_scan_status;
    }
    return MemMetaImpl::VisitPendingDeletes(visitor);
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
    volume::VolumeImpl::Initialize();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    auto meta = std::make_unique<FailingScanMetaEngine>();
    meta_ = meta.get();
    volume::VolumeImpl::Instance().set_meta_engine(std::move(meta));
    volume::VolumeImpl::Instance().set_data_engine(std::make_unique<RecordingDataEngine>());
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
  EXPECT_EQ(status.code(), Status::kIOError) << status.message();
  EXPECT_EQ(status.message(), "orphan scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesPendingDeleteScanFailure) {
  meta_->pending_delete_scan_status = Status::IOError("pending delete scan unavailable");

  const auto status = Reclaimer::Instance().Reconcile();
  EXPECT_EQ(status.code(), Status::kIOError) << status.message();
  EXPECT_EQ(status.message(), "pending delete scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesPendingScanFailure) {
  // The orphan scan is healthy; the second visitor's failure must reach the
  // caller just the same.
  meta_->pending_scan_status = Status::IOError("pending scan unavailable");

  const auto status = Reclaimer::Instance().Reconcile();
  EXPECT_EQ(status.code(), Status::kIOError) << status.message();
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
