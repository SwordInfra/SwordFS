// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandle — the fh → FileHandle mapping and the
// high-level Open/Create entry points.

#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"
#include "vfs/DirHandle.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/InodeHandle.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::RenameResult;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::metadata::UnlinkResult;
using swordfs::utils::Status;

// Minimal no-op data engine. The fixture needs to install one so the
// CHECK(data_) in InodeHandle::ReclaimData does not fire on the
// orphan-close reclaim path; the full FakeDataEngine (which records
// Delete calls) lives below alongside the ReclaimData tests.
class NoopDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  swordfs::storage::DataEngineLimits Limits() const override {
    return {};
  }
  bool Head(std::string_view, size_t *) override {
    return false;
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }
};

// Minimal IMetaEngine — every op succeeds; Create fabricates an inode.
class MockMetaEngine : public IMetaEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status FormatVolume(const SwordFsVolume &) override {
    return Status::OK();
  }
  Status LoadVolume(SwordFsVolume *) override {
    return Status::OK();
  }
  Limits GetLimits() const override {
    return {};
  }
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID, SwordFsInode *out) override {
    // The reclaim path no longer consults inode metadata; this remains for
    // tests that model an inode's attributes (ownership, nlink).
    if (out) {
      *out = {};
    }
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = next_ino_++;
      out->attr.ino = out->ino;
      out->attr.mode = S_IFREG | 0644;
    }
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view, UnlinkResult *) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult *) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) override {
    return Status::OK();
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Access(InodeID, uint32_t) override {
    return Status::OK();
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID) override {
    return open_status;
  }
  Status PrepareReclaim(InodeID ino, swordfs::metadata::ReclaimWork *work) override {
    ++prepare_reclaim_calls;
    last_reclaim_ino = ino;
    if (!prepare_reclaim_status.ok()) {
      return prepare_reclaim_status;
    }
    if (work != nullptr) {
      work->ino = ino;
      for (const auto &chunk : reclaim_chunks) {
        // Mirror the real engines: the frozen identity is derived once, at
        // freeze time, from the authoritative descriptor.
        work->chunks.push_back(
            swordfs::metadata::ReclaimChunk{chunk, chunk::FormatChunkObjectKey(ino, chunk.index, chunk.revision)});
      }
    }
    return Status::OK();
  }
  Status CompleteReclaim(InodeID ino) override {
    ++reclaim_calls;
    last_reclaim_ino = ino;
    return reclaim_status;
  }
  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status AllocateChunkRevision(swordfs::metadata::ChunkRevision *revision) override {
    if (revision == nullptr) {
      return Status::InvalidArgument("chunk revision output is null");
    }
    *revision = next_revision_++;
    return Status::OK();
  }
  Status VisitChunks(InodeID, const swordfs::metadata::ChunkVisitorFn &) override {
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status CommitChunk(InodeID, const std::optional<SwordFsChunk> &, const SwordFsChunk &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, ChunkIndex, SwordFsChunk *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, uint64_t size) override {
    ++truncate_calls;
    last_truncate_size = size;
    return truncate_status;
  }

  // Observable state / injectable statuses for tests.
  int truncate_calls = 0;
  size_t last_truncate_size = 0;
  // Number of completed reclaims (CompleteReclaim calls).
  int reclaim_calls = 0;
  int prepare_reclaim_calls = 0;
  InodeID last_reclaim_ino = 0;
  Status open_status = Status::OK();
  Status truncate_status = Status::OK();
  Status reclaim_status = Status::OK();
  Status prepare_reclaim_status = Status::OK();
  // Frozen descriptors the mock hands out from PrepareReclaim.
  std::vector<swordfs::metadata::SwordFsChunk> reclaim_chunks;

 private:
  InodeID next_ino_ = 1000;
  swordfs::metadata::ChunkRevision next_revision_ = 1;
};

class FileHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Volume lifecycle is control-plane work and follows the production
    // topology: initialize it on the gtest POSIX thread before entering a
    // fiber for runtime-only state reset.
    volume::VolumeImpl::Initialize();
    swordfs::test::RunInTestFiber([&] {
      // Drop any per-inode state left by a prior test. The
      // InodeHandleManager is fiber-owned runtime state.
      InodeHandleManager::Instance().Initialize();
    });

    auto meta = std::make_unique<MockMetaEngine>();
    mock_meta_ = meta.get();
    volume::VolumeImpl::Instance().set_meta_engine(std::move(meta));
    // ReclaimData now requires a data engine (production invariant —
    // --bucket is required). Tests that exercise the orphan-close reclaim
    // path install a no-op fake here.
    volume::VolumeImpl::Instance().set_data_engine(std::make_unique<NoopDataEngine>());
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] {
      // Clean up any handles left by a test.
      for (uint64_t fh : fhs_) {
        if (auto handle = HandleManager::Instance().FindAs<FileHandle>(fh)) {
          handle->Release();
        } else if (auto handle = HandleManager::Instance().FindAs<DirHandle>(fh)) {
          handle->Release();
        }
      }
    });
    // Destroy/reset injected engines on the POSIX test thread.
    volume::VolumeImpl::Initialize();
  }

  /// Helper: open a handle and remember it for cleanup.
  uint64_t OpenHandle(metadata::InodeID ino) {
    std::shared_ptr<FileHandle> handle;
    auto status = FileHandle::Open(ino, 0, &handle);
    EXPECT_TRUE(status.ok());
    uint64_t fh = handle->fh();
    fhs_.push_back(fh);
    return fh;
  }

  std::vector<uint64_t> fhs_;
  MockMetaEngine *mock_meta_ = nullptr;
};

// ────────────────────────────────────────────────────────────────
// Singleton
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, InstanceIsSingleton) {
  auto &a = HandleManager::Instance();
  auto &b = HandleManager::Instance();
  EXPECT_EQ(&a, &b);
}

// ────────────────────────────────────────────────────────────────
// Open + Find
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, OpenAndFind) {
  uint64_t fh = OpenHandle(42);

  auto found = HandleManager::Instance().FindAs<FileHandle>(fh);
  EXPECT_NE(found, nullptr);
}

FIBER_TEST_F(FileHandleTest, FindNonexistent) {
  auto found = HandleManager::Instance().FindAs<FileHandle>(999);
  EXPECT_EQ(found, nullptr);
}

FIBER_TEST_F(FileHandleTest, OpenMultipleHandles) {
  uint64_t fh1 = OpenHandle(10);
  uint64_t fh2 = OpenHandle(20);
  EXPECT_NE(fh1, fh2);

  auto f1 = HandleManager::Instance().FindAs<FileHandle>(fh1);
  auto f2 = HandleManager::Instance().FindAs<FileHandle>(fh2);

  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);
  EXPECT_NE(f1->handle().get(), f2->handle().get());
}

// ────────────────────────────────────────────────────────────────
// HandleManager unregister
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, ReleaseRemovesHandle) {
  uint64_t fh = OpenHandle(7);
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  ASSERT_NE(handle, nullptr);
  ASSERT_TRUE(handle->Release().ok());

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh), nullptr);
}

FIBER_TEST_F(FileHandleTest, ReleaseKeepsOtherHandles) {
  uint64_t fh1 = OpenHandle(1);
  uint64_t fh2 = OpenHandle(2);
  auto f1 = HandleManager::Instance().FindAs<FileHandle>(fh1);
  ASSERT_NE(f1, nullptr);
  ASSERT_TRUE(f1->Release().ok());

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh1), nullptr);
  auto f2 = HandleManager::Instance().FindAs<FileHandle>(fh2);
  ASSERT_NE(f2, nullptr);
}

// ────────────────────────────────────────────────────────────────
// Open duplicate fh
// ────────────────────────────────────────────────────────────────

// With auto-allocated fh, duplicates cannot occur — every Open gets a
// unique handle.
FIBER_TEST_F(FileHandleTest, OpenReturnsUniqueFh) {
  uint64_t fh1 = OpenHandle(100);
  uint64_t fh2 = OpenHandle(200);
  EXPECT_NE(fh1, fh2);
}

// ────────────────────────────────────────────────────────────────
// Shared ownership — Find keeps handle alive across Unregister
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, FindKeepsHandleAliveAfterUnregister) {
  uint64_t fh = OpenHandle(55);

  // Hold a shared_ptr before releasing.
  auto held = HandleManager::Instance().FindAs<FileHandle>(fh);
  ASSERT_NE(held, nullptr);

  ASSERT_TRUE(held->Release().ok());
  // Map entry is gone.
  auto after = HandleManager::Instance().FindAs<FileHandle>(fh);
  EXPECT_EQ(after, nullptr);
  ASSERT_NE(held, nullptr);
  EXPECT_NE(held->handle().get(), nullptr);  // still alive
}

// ────────────────────────────────────────────────────────────────
// Concurrency — basic multi-threaded access
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, ConcurrentOpenAndFind) {
  constexpr int kThreads = 4;
  constexpr int kIters = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.push_back(swordfs::test::StartFiberTestThread([t] {
      for (int i = 0; i < kIters; ++i) {
        auto ino = static_cast<metadata::InodeID>(t * kIters + i + 100);
        std::shared_ptr<FileHandle> handle;
        auto status = FileHandle::Open(ino, 0, &handle);
        EXPECT_TRUE(status.ok());
        auto found = HandleManager::Instance().FindAs<FileHandle>(handle->fh());
        EXPECT_NE(found, nullptr);
        // Release eagerly: leaving 400 fh dangling across the test
        // boundary corrupts later open-count assertions (Get(ino, false)
        // would hand out handles with a non-zero open_count for these
        // synthetic inodes).
        ASSERT_TRUE(handle->Release().ok());
      }
    }));
  }
  for (auto &th : threads) {
    th.join();
  }
}

class TestDirIterator final : public metadata::DirIterator {
 public:
  Status Seek(uint64_t) override {
    return Status::OK();
  }

  Status Peek(metadata::SwordFsEntry *, uint64_t *) override {
    return Status::EndOfDirectory("directory end");
  }

  void Advance() override {
  }
};

metadata::DirIteratorPtr NewTestDirIterator() {
  return std::make_shared<TestDirIterator>();
}

// ────────────────────────────────────────────────────────────────
// Generic file and directory handles
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, RegisterAndFindDirectoryHandle) {
  auto handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t fh = HandleManager::Instance().Register(handle);

  auto found = HandleManager::Instance().FindAs<DirHandle>(fh);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found, handle);
  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, RegisterAssignsUniqueHandlesAcrossTypes) {
  std::shared_ptr<FileHandle> opened_file;
  ASSERT_TRUE(FileHandle::Open(9007, 0, &opened_file).ok());
  auto file_handle = HandleManager::Instance().FindAs<FileHandle>(opened_file->fh());
  ASSERT_NE(file_handle, nullptr);

  auto dir_handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t dir_fh = HandleManager::Instance().Register(dir_handle);

  EXPECT_NE(opened_file->fh(), dir_fh);
  EXPECT_EQ(file_handle->fh(), opened_file->fh());
  EXPECT_EQ(dir_handle->fh(), dir_fh);

  ASSERT_TRUE(opened_file->Release().ok());
  ASSERT_TRUE(dir_handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, FindAsRejectsWrongHandleType) {
  auto handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t fh = HandleManager::Instance().Register(handle);

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh), nullptr);
  EXPECT_NE(HandleManager::Instance().FindAs<DirHandle>(fh), nullptr);
  ASSERT_TRUE(handle->Release().ok());
}

// ────────────────────────────────────────────────────────────────
// FileHandle::Open error propagation
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, OpenMetaFailurePropagates) {
  mock_meta_->open_status = Status::Permission("denied");
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, 0, &handle);
  EXPECT_TRUE(status.IsPermission());
}

FIBER_TEST_F(FileHandleTest, OpenTruncateAppliesOTrunc) {
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  ASSERT_TRUE(status.ok());
  fhs_.push_back(handle->fh());
  EXPECT_EQ(mock_meta_->truncate_calls, 1);
  EXPECT_EQ(mock_meta_->last_truncate_size, 0u);
}

FIBER_TEST_F(FileHandleTest, OpenTruncateFailurePropagates) {
  mock_meta_->truncate_status = Status::Internal("truncate failed");
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), Status::kInternal);
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, InodeHandleGetMissingWithoutCreate) {
  EXPECT_EQ(InodeHandleManager::Instance().Get(9001, /*create_if_missing=*/false), nullptr);
}

FIBER_TEST_F(FileHandleTest, InodeHandleGetExistingTracksOpenCount) {
  uint64_t fh = OpenHandle(9002);
  auto inode_handle = InodeHandleManager::Instance().Get(9002, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->ino(), 9002);
  EXPECT_EQ(inode_handle->open_count(), 1);
}

FIBER_TEST_F(FileHandleTest, InodeHandleRecreatedAfterExpiry) {
  {
    auto inode_handle = InodeHandleManager::Instance().Get(9003, true);
    ASSERT_NE(inode_handle, nullptr);
  }
  // All shared references dropped — the weak entry is expired, so a fresh
  // InodeHandle must be created on the next lookup.
  auto recreated = InodeHandleManager::Instance().Get(9003, true);
  ASSERT_NE(recreated, nullptr);
  EXPECT_EQ(recreated->ino(), 9003);
}

// ────────────────────────────────────────────────────────────────
// InodeHandle open-unlink reclaim
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, CloseReclaimsOrphanedInode) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(9004, 0, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9004, false);
  ASSERT_NE(inode_handle, nullptr);
  // ReclaimData sees the open descriptor and records local deferral; durable
  // orphan authority belongs to metadata, while cleanup is deferred to the
  // close below.
  ASSERT_TRUE(inode_handle->ReclaimData().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 0) << "an open descriptor must defer the reclaim";

  ASSERT_TRUE(handle->Release().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);
}

FIBER_TEST_F(FileHandleTest, CloseToleratesDeferredReclaimFailureAndLeavesFenceRetryable) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(9007, 0, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9007, false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->ReclaimData().ok());

  mock_meta_->reclaim_status = Status::IOError("injected deferred reclaim failure");
  EXPECT_TRUE(handle->Release().ok()) << "close reports flush status, not deferred reclaim status";
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);

  // ReclaimWithFence must release the local fence even when completion fails,
  // otherwise durable reconciliation could never retry this inode.
  mock_meta_->reclaim_status = Status::OK();
  EXPECT_TRUE(inode_handle->ReclaimData().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 2);
}

FIBER_TEST_F(FileHandleTest, CloseOnlyReclaimsOnLastReference) {
  std::shared_ptr<FileHandle> h1, h2;
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h1).ok());
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h2).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9005, false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->ReclaimData().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 0) << "two descriptors still hold the inode";

  // First close: open count 2 → 1 — no reclaim yet.
  ASSERT_TRUE(h1->Release().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 0);
  EXPECT_EQ(inode_handle->open_count(), 1);

  // Final close: open count 1 → 0 — reclaim now.
  ASSERT_TRUE(h2->Release().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);
  EXPECT_EQ(inode_handle->open_count(), 0);
}

FIBER_TEST_F(FileHandleTest, ReclaimDataReclaimsImmediatelyWhenNoFdIsOpen) {
  // The complement of the deferral: with no descriptor open there is no later
  // Close to defer to, so ReclaimData reclaims on the spot.
  auto inode_handle = InodeHandleManager::Instance().Get(9006, true);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->open_count(), 0);

  ASSERT_TRUE(inode_handle->ReclaimData().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);
}

// ────────────────────────────────────────────────────────────────
// InodeHandle open-fd tracking (VfsImpl::Unlink decision)
// ────────────────────────────────────────────────────────────────
//
// `VfsImpl::Unlink` consults the per-inode InodeHandle (via
// InodeHandleManager::Get) to decide whether to defer the inode
// reclaim. The case below exercises the lifecycle: no handle → open fd
// → handle present with open_count>0 → close → open_count back to 0.

FIBER_TEST_F(FileHandleTest, InodeHandleOpenFdTracking) {
  // No handle yet -> Get without create reports absence.
  InodeID test_ino = 9998;
  EXPECT_EQ(InodeHandleManager::Instance().Get(test_ino, false), nullptr);

  // FileHandle::Open routes through mock_meta_ and creates the handle.
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(test_ino, O_RDWR, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(test_ino, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_GT(inode_handle->open_count(), 0u);

  // Release drops the open fd; the handle's open_count reflects it.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_EQ(inode_handle->open_count(), 0u);
}

// ────────────────────────────────────────────────────────────────
// InodeHandle::ReclaimData — coordinator contract
// ────────────────────────────────────────────────────────────────
//
// ReclaimData claims the per-inode reclaim fence and then fans a single call
// out to both engines: the metadata engine freezes the inode's object
// identities (PrepareReclaim) and drops the live inode, the data engine
// deletes exactly those frozen keys, and only then does the metadata engine
// drop the frozen record (CompleteReclaim). These tests assert that the call
// sequence matches the documented contract; the per-engine behaviour is
// exercised by the metadata- and data-engine unit tests.

namespace {

// Mock data engine: records every Delete call and lets the test inject
// per-key failure responses.
class FakeDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  swordfs::storage::DataEngineLimits Limits() const override {
    return {};
  }
  bool Head(std::string_view, size_t *) override {
    return false;
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    if (put_entered_ != nullptr) {
      auto *entered = put_entered_;
      auto *release = put_release_;
      put_entered_ = nullptr;
      put_release_ = nullptr;
      entered->post();
      release->wait();
    }
    return put_status;
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view key) override {
    delete_calls.push_back(std::string(key));
    auto it = fail_keys.find(std::string(key));
    if (it != fail_keys.end()) {
      return it->second;
    }
    return Status::OK();
  }

  void BlockNextPut(folly::fibers::Baton *entered, folly::fibers::Baton *release) {
    put_entered_ = entered;
    put_release_ = release;
  }

  std::vector<std::string> delete_calls;
  std::unordered_map<std::string, Status> fail_keys;
  Status put_status = Status::OK();

 private:
  folly::fibers::Baton *put_entered_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
};

// Mock metadata engine: records VisitChunks / ReclaimInode invocations
// and returns a configurable chunk list + GetAttr (so the ReclaimData
// guard sees the nlink value the test wants).
class TrackingMetaEngine final : public swordfs::metadata::IMetaEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status FormatVolume(const SwordFsVolume &) override {
    return Status::OK();
  }
  Status LoadVolume(SwordFsVolume *) override {
    return Status::OK();
  }
  Limits GetLimits() const override {
    return {};
  }
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    auto it = attrs.find(ino);
    if (it == attrs.end()) {
      return Status::NotFound("inode not found");
    }
    if (out) {
      *out = {};
      out->ino = ino;
      out->attr.ino = it->second.st_ino;
      out->attr.mode = it->second.st_mode;
      out->attr.nlink = it->second.st_nlink;
      out->attr.uid = it->second.st_uid;
      out->attr.gid = it->second.st_gid;
      out->attr.size = it->second.st_size;
      out->attr.blksize = it->second.st_blksize;
      out->attr.blocks = it->second.st_blocks;
      out->attr.atime = it->second.st_atime;
      out->attr.atime_nsec = it->second.st_atim.tv_nsec;
      out->attr.mtime = it->second.st_mtime;
      out->attr.mtime_nsec = it->second.st_mtim.tv_nsec;
      out->attr.ctime = it->second.st_ctime;
      out->attr.ctime_nsec = it->second.st_ctim.tv_nsec;
    }
    return Status::OK();
  }
  void SetAttr(InodeID ino, struct stat attr) {
    attrs[ino] = attr;
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view, UnlinkResult *) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult *) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) override {
    return Status::OK();
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Access(InodeID, uint32_t) override {
    return Status::OK();
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID) override {
    if (open_entered_ != nullptr) {
      auto *entered = open_entered_;
      auto *release = open_release_;
      open_entered_ = nullptr;
      open_release_ = nullptr;
      entered->post();
      release->wait();
    }
    return open_status;
  }
  Status PrepareReclaim(InodeID ino, swordfs::metadata::ReclaimWork *work) override {
    ++prepare_reclaim_calls;
    last_reclaim_ino = ino;
    if (!prepare_reclaim_status.ok()) {
      return prepare_reclaim_status;
    }
    if (prepare_entered_ != nullptr) {
      // Park inside the engine. By the time a caller is here the inode's
      // reclaim fence is already claimed, which is exactly the window the
      // fence has to protect.
      auto *entered = prepare_entered_;
      auto *release = prepare_release_;
      prepare_entered_ = nullptr;
      prepare_release_ = nullptr;
      entered->post();
      release->wait();
    }
    if (work != nullptr) {
      work->ino = ino;
      for (const auto &chunk : chunks) {
        work->chunks.push_back(
            swordfs::metadata::ReclaimChunk{chunk, chunk::FormatChunkObjectKey(ino, chunk.index, chunk.revision)});
      }
    }
    return Status::OK();
  }
  Status CompleteReclaim(InodeID ino) override {
    ++complete_reclaim_calls;
    last_reclaim_ino = ino;
    return reclaim_status;
  }
  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status AllocateChunkRevision(swordfs::metadata::ChunkRevision *revision) override {
    if (revision == nullptr) {
      return Status::InvalidArgument("chunk revision output is null");
    }
    *revision = next_revision_++;
    return Status::OK();
  }
  Status VisitChunks(InodeID ino, const swordfs::metadata::ChunkVisitorFn &visitor) override {
    ++visit_chunks_calls;
    last_visit_ino = ino;
    if (!visit_chunks_status.ok()) {
      return visit_chunks_status;
    }
    for (const auto &chunk : chunks) {
      auto status = visitor(chunk);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status CommitChunk(InodeID, const std::optional<swordfs::metadata::SwordFsChunk> &,
                     const swordfs::metadata::SwordFsChunk &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, swordfs::metadata::ChunkIndex, swordfs::metadata::SwordFsChunk *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, uint64_t) override {
    return Status::OK();
  }

  int visit_chunks_calls = 0;
  int prepare_reclaim_calls = 0;
  int complete_reclaim_calls = 0;
  InodeID last_visit_ino = 0;
  InodeID last_reclaim_ino = 0;
  Status visit_chunks_status = Status::OK();
  Status prepare_reclaim_status = Status::OK();
  Status reclaim_status = Status::OK();
  Status open_status = Status::OK();
  std::vector<swordfs::metadata::SwordFsChunk> chunks;
  std::unordered_map<InodeID, struct stat> attrs;

  // Park the next PrepareReclaim call inside the engine so a test can
  // observe the reclaim window (the fence is claimed by then).
  void BlockNextPrepare(folly::fibers::Baton *entered, folly::fibers::Baton *release) {
    prepare_entered_ = entered;
    prepare_release_ = release;
  }

  void BlockNextOpen(folly::fibers::Baton *entered, folly::fibers::Baton *release) {
    open_entered_ = entered;
    open_release_ = release;
  }

 private:
  swordfs::metadata::ChunkRevision next_revision_ = 1;
  folly::fibers::Baton *open_entered_{nullptr};
  folly::fibers::Baton *open_release_{nullptr};
  folly::fibers::Baton *prepare_entered_{nullptr};
  folly::fibers::Baton *prepare_release_{nullptr};
};

// Reclaim an inode through the InodeHandle API. The manager is now only
// a registry, so callers fetch (or lazily create) the per-inode handle
// and invoke its ReclaimData() instance method directly.
static Status ReclaimInode(metadata::InodeID ino) {
  auto handle = InodeHandleManager::Instance().Get(ino,
                                                   /*create_if_missing=*/true);
  if (!handle) {
    return Status::Internal("failed to get or create InodeHandle");
  }
  return handle->ReclaimData();
}

void ResetVolumeFromFiberForTest() {
  swordfs::test::RunInTestThreadFromFiber([] { swordfs::volume::VolumeImpl::Initialize(); });
}

FIBER_TEST_F(FileHandleTest, ReclaimDataDeletesEveryFrozenChunkAndCompletes) {
  // Reset lifecycle state on the POSIX test worker, not on this fiber.
  ResetVolumeFromFiberForTest();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  // Keep raw pointers around for assertions after the engines move into
  // the volume singleton (which takes ownership).
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  // Seed two chunk records. ReclaimData must delete exactly the identities
  // the metadata engine freezes from these descriptors.
  swordfs::metadata::SwordFsChunk c0{};
  c0.index = 0;
  c0.start_offset = 0;
  c0.revision = 1;
  c0.size = 1024;
  swordfs::metadata::SwordFsChunk c1{};
  c1.index = 1;
  c1.start_offset = 65536;
  c1.revision = 2;
  c1.size = 2048;
  meta->chunks = {c0, c1};

  // Install engines in the volume singleton so ReclaimData can find them.
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(4242).ok());

  // The mock freezes chunks in insertion order.
  EXPECT_EQ(data->delete_calls.size(), 2);
  EXPECT_EQ(data->delete_calls[0], chunk::FormatChunkObjectKey(4242, 0, 1));
  EXPECT_EQ(data->delete_calls[1], chunk::FormatChunkObjectKey(4242, 1, 2));

  // Preparation and completion happen exactly once each, for this inode.
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
  EXPECT_EQ(meta->last_reclaim_ino, 4242);
}

FIBER_TEST_F(FileHandleTest, ReclaimDataRetriesIdempotentDeletesUntilComplete) {
  // A failed object delete must keep the frozen record (CompleteReclaim is
  // NOT called), and a later attempt must replay the same frozen identities
  // and complete once every delete succeeds.
  ResetVolumeFromFiberForTest();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  swordfs::metadata::SwordFsChunk c{};
  c.index = 0;
  c.revision = 1;
  meta->chunks = {c};
  data->fail_keys[chunk::FormatChunkObjectKey(99, 0, 1)] = Status::IOError("injected delete failure");

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  const auto first = ReclaimInode(99);
  EXPECT_FALSE(first.ok());
  EXPECT_EQ(data->delete_calls.size(), 1u);
  EXPECT_EQ(meta->complete_reclaim_calls, 0) << "the frozen record must survive a failed delete";

  data->fail_keys.clear();
  ASSERT_TRUE(ReclaimInode(99).ok());
  EXPECT_EQ(data->delete_calls.size(), 2u);
  EXPECT_EQ(data->delete_calls[1], chunk::FormatChunkObjectKey(99, 0, 1));
  EXPECT_EQ(meta->prepare_reclaim_calls, 2);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
}

FIBER_TEST_F(FileHandleTest, ReclaimDataCompletesWithoutDeletesWhenNoChunksFrozen) {
  // An inode with zero registered chunks (empty file, symlink) still has to
  // be dropped from the metadata engine: preparation freezes an empty work
  // list and completion removes the record. A data engine is still required
  // (see the CHECK in InodeHandle) even though no Delete is issued.
  ResetVolumeFromFiberForTest();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(123).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
  EXPECT_TRUE(data->delete_calls.empty());
}

FIBER_TEST_F(FileHandleTest, ReclaimDataAttemptsEveryChunkThenKeepsRecordOnFailure) {
  // A failing per-chunk Delete must not stop the sweep: every frozen object
  // gets attempted, and because one failed the frozen record survives so the
  // retry can finish the (idempotent) deletes.
  ResetVolumeFromFiberForTest();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  swordfs::metadata::SwordFsChunk c0{};
  c0.index = 0;
  c0.revision = 1;
  swordfs::metadata::SwordFsChunk c1{};
  c1.index = 1;
  c1.revision = 2;
  meta->chunks = {c0, c1};
  data->fail_keys[chunk::FormatChunkObjectKey(1, 0, 1)] = Status::Internal("forced");

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  const auto st = ReclaimInode(1);
  EXPECT_FALSE(st.ok());
  // Both chunks were attempted despite the failure on "1/0/1".
  EXPECT_EQ(data->delete_calls.size(), 2);
  EXPECT_EQ(meta->complete_reclaim_calls, 0) << "a failed delete must keep the frozen record";
}

FIBER_TEST_F(FileHandleTest, ReclaimDataPropagatesPrepareFailure) {
  // If the metadata engine refuses to prepare the reclaim (I/O error, a
  // corrupt frozen record), nothing may be deleted and nothing may complete:
  // the inode is still live metadata and its objects are still referenced.
  ResetVolumeFromFiberForTest();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();
  meta->prepare_reclaim_status = Status::Internal("nope");

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  const auto st = ReclaimInode(42);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty());
}

}  // namespace

// ────────────────────────────────────────────────────────────────
// ReclaimData guards — last-line-of-defence against stale-state callers.
// ────────────────────────────────────────────────────────────────

namespace {

// Install engines with an ino whose nlink is whatever the test wants.
// Returns raw pointers to both engines for assertions.
struct Engines {
  TrackingMetaEngine *meta;
  FakeDataEngine *data;
};

Engines InstallEnginesForInode(InodeID ino, nlink_t nlink) {
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  struct stat attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.st_nlink = nlink;
  meta->SetAttr(ino, attr);

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));
  vol.set_chunk_size_for_test(4096);
  return {meta, data};
}

}  // namespace

FIBER_TEST_F(FileHandleTest, ReclaimDataReleasesFenceWhenInodeIsNotReclaimable) {
  // A Link that lands before the reclaim's metadata transaction turns
  // preparation into "not reclaimable": the engine changes nothing, so
  // ReclaimData must delete nothing and complete nothing — and it must
  // release its fence so the revived inode stays openable.
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/2);
  // Add a chunk so we'd have something to delete if the guard fell
  // through; the absence of any delete is what we actually verify.
  swordfs::metadata::SwordFsChunk chunk{};
  chunk.index = 0;
  chunk.revision = 1;
  meta->chunks = {chunk};
  meta->prepare_reclaim_status = Status::NotFound("inode is still linked");

  // Hold the handle across the reclaim: the registry only keeps weak
  // references, so the reclaimed inode's handle must stay alive to be
  // inspected afterwards.
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);

  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_TRUE(data->delete_calls.empty()) << "ReclaimData must NOT delete chunk objects when the reclaim was "
                                             "not prepared";
  EXPECT_EQ(meta->complete_reclaim_calls, 0);
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);

  ASSERT_TRUE(inode_handle->Open(O_RDONLY).ok()) << "the fence must be released after a declined reclaim";
  ASSERT_TRUE(inode_handle->Close().ok());
}

FIBER_TEST_F(FileHandleTest, ReclaimDataFencesOpensUntilTheInodeIsFrozen) {
  // While a reclaim owns the inode — between claiming the fence and the
  // metadata point of no return — an open must fail rather than gain a
  // descriptor on an inode whose objects are about to be deleted.
  ResetVolumeFromFiberForTest();
  auto engines = InstallEnginesForInode(7, /*nlink=*/0);
  auto *meta = engines.meta;

  folly::fibers::Baton prepare_entered;
  folly::fibers::Baton prepare_release;
  meta->BlockNextPrepare(&prepare_entered, &prepare_release);

  std::atomic<bool> reclaim_ok{false};
  auto reclaiming_thread = swordfs::test::StartFiberTestThread([&] { reclaim_ok.store(ReclaimInode(7).ok()); });

  // Wait until the reclaim is parked inside PrepareReclaim, holding the fence.
  prepare_entered.wait();

  std::shared_ptr<FileHandle> handle;
  const auto open_status = FileHandle::Open(7, O_RDONLY, &handle);
  EXPECT_TRUE(open_status.IsNotFound()) << "an open must not slip in while the inode is being reclaimed";

  prepare_release.post();
  reclaiming_thread.join();
  EXPECT_TRUE(reclaim_ok.load());
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
}

FIBER_TEST_F(FileHandleTest, ConcurrentOpenSucceedsWhileLastCloseFlushesLinkedInode) {
  // A normal linked inode must remain openable while its last descriptor is
  // closing. The closing descriptor is still a live reference until Flush
  // finishes; using the reclaim fence for this window would incorrectly turn
  // an ordinary concurrent Open into NotFound.
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)meta;

  std::shared_ptr<FileHandle> closing_handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &closing_handle).ok());
  auto payload = folly::IOBuf::copyBuffer("x");
  ASSERT_TRUE(closing_handle->Write(*payload, 0).ok());

  folly::fibers::Baton put_entered;
  folly::fibers::Baton put_release;
  data->BlockNextPut(&put_entered, &put_release);

  std::atomic<bool> close_ok{false};
  auto closing_thread = swordfs::test::StartFiberTestThread([&] { close_ok.store(closing_handle->Release().ok()); });
  put_entered.wait();  // the last close is now blocked inside Flush/Put

  std::shared_ptr<FileHandle> reopened;
  const auto open_status = FileHandle::Open(7, O_RDONLY, &reopened);
  EXPECT_TRUE(open_status.ok()) << open_status.message();
  ASSERT_NE(reopened, nullptr);

  put_release.post();
  closing_thread.join();
  EXPECT_TRUE(close_ok.load());

  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->open_count(), 1U);
  ASSERT_TRUE(reopened->Release().ok());
}

FIBER_TEST_F(FileHandleTest, FailedLastCloseFlushLeavesOrphanForRetry) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("x");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  // Reconciliation/unlink observes the live descriptor and records local
  // deferral instead of reclaiming under the writer.
  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);

  data->put_status = Status::IOError("injected flush failure");
  const auto close_status = handle->Release();
  EXPECT_EQ(close_status.code(), Status::kIOError);
  EXPECT_EQ(meta->prepare_reclaim_calls, 0) << "failed flush must not reclaim potentially unpublished data";

  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->open_count(), 0U) << "the descriptor is closed even when its flush fails";

  // Durable orphan metadata is the recovery authority after a failed close.
  // Model the next reconciliation pass after the data plane recovers.
  data->put_status = Status::OK();
  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
}

FIBER_TEST_F(FileHandleTest, FailedOpenReleasesItsReferenceWithoutReclaimingLinkedInode) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)data;
  meta->open_status = Status::Permission("denied");

  // Hold the shared per-inode object explicitly. The manager registry owns
  // only a weak_ptr, so after a failed FileHandle::Open there is otherwise no
  // requirement that an expired registry entry remain observable.
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->open_count(), 0U);

  std::shared_ptr<FileHandle> handle;
  const auto status = FileHandle::Open(7, O_RDONLY, &handle);
  EXPECT_EQ(status.code(), Status::kPermission);
  EXPECT_EQ(handle, nullptr);

  EXPECT_EQ(inode_handle->open_count(), 0U);
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);
}

FIBER_TEST_F(FileHandleTest, FailedSecondOpenReleasesOnlyItsOwnReference) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)data;

  std::shared_ptr<FileHandle> first;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &first).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_EQ(inode_handle->open_count(), 1U);

  meta->open_status = Status::Permission("denied");
  std::shared_ptr<FileHandle> second;
  const auto status = FileHandle::Open(7, O_RDONLY, &second);
  EXPECT_EQ(status.code(), Status::kPermission);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(inode_handle->open_count(), 1U) << "failed open must release only the reference it acquired";
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);

  ASSERT_TRUE(first->Release().ok());
}

FIBER_TEST_F(FileHandleTest, FailedOpenAfterOrphaningAttemptsReclaimAndReleasesFenceOnFailure) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);
  (void)data;

  folly::fibers::Baton open_entered;
  folly::fibers::Baton open_release;
  meta->open_status = Status::Permission("denied");
  meta->BlockNextOpen(&open_entered, &open_release);

  std::atomic<int> open_code{Status::kOk};
  auto opening_thread = swordfs::test::StartFiberTestThread([&] {
    std::shared_ptr<FileHandle> handle;
    const auto status = FileHandle::Open(7, O_RDONLY, &handle);
    open_code.store(status.code());
  });

  // The failed open already owns a temporary descriptor reference but is
  // still parked inside the metadata check. Reconciliation must defer and
  // mark that handle orphaned rather than reclaiming under it.
  open_entered.wait();
  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);

  // When the metadata check finally fails, releasing the last temporary
  // reference must inherit the orphan cleanup. Inject a reclaim failure to
  // prove the local fence is still released for the durable retry path.
  meta->prepare_reclaim_status = Status::IOError("injected reclaim failure after failed open");
  open_release.post();
  opening_thread.join();
  EXPECT_EQ(open_code.load(), Status::kPermission);
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 0);

  meta->prepare_reclaim_status = Status::OK();
  ASSERT_TRUE(ReclaimInode(7).ok()) << "failed-open cleanup must not leak the reclaim fence";
  EXPECT_EQ(meta->prepare_reclaim_calls, 2);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
}

FIBER_TEST_F(FileHandleTest, ReclaimDataDefersToAFenceOwnedByAnotherReclaim) {
  // A reclaim already owns the inode's fence — here it is parked inside
  // PrepareReclaim, i.e. past the point where the fence was claimed, with the
  // inode not yet frozen. A second ReclaimData must not start a competing
  // prepare/delete sequence: it returns OK and leaves cleanup to the reclaim
  // that owns the fence; durable retry state remains in metadata.
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);
  swordfs::metadata::SwordFsChunk chunk{};
  chunk.index = 0;
  chunk.revision = 1;
  meta->chunks = {chunk};

  folly::fibers::Baton prepare_entered;
  folly::fibers::Baton prepare_release;
  meta->BlockNextPrepare(&prepare_entered, &prepare_release);

  std::atomic<bool> first_ok{false};
  auto first = swordfs::test::StartFiberTestThread([&] { first_ok.store(ReclaimInode(7).ok()); });
  prepare_entered.wait();  // the first reclaim owns the fence from here on
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);

  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->ReclaimData().ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1) << "a fenced inode must not be prepared twice";
  EXPECT_EQ(meta->complete_reclaim_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty());

  prepare_release.post();
  first.join();
  EXPECT_TRUE(first_ok.load());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
  EXPECT_EQ(data->delete_calls.size(), 1U);
}

FIBER_TEST_F(FileHandleTest, ReclaimDataRefusesWhileAnOpenHandleHoldsTheInode) {
  // A descriptor still holds the inode, so ReclaimData must defer: the open
  // fd's reads keep working, and until the last Close the engines see no
  // reclaim activity at all.
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);

  // Open a file handle on this inode. The tracking engine lets
  // everything through, so this just installs a tracked fh.
  std::shared_ptr<FileHandle> fh;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &fh).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(7, false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_GT(inode_handle->open_count(), 0u);

  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);
  EXPECT_EQ(meta->complete_reclaim_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty()) << "ReclaimData must NOT delete chunk objects while a fd is open";

  // Cleanup: releasing the last descriptor runs the deferred reclaim, so the
  // engines must now see exactly one prepare/complete pair.
  ASSERT_TRUE(fh->Release().ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 1);
}

FIBER_TEST_F(FileHandleTest, RevivedInodeDoesNotKeepLocalOrphanDeferralAfterLastClose) {
  // An unlink can mark the local handle orphaned while a descriptor is open,
  // but a concurrent Link may revive the inode before the last Close reaches
  // PrepareReclaim. Once metadata declines that reclaim, the per-handle
  // deferral must be consumed: later ordinary closes of the revived inode
  // must not repeatedly enter the reclaim path or transiently fence opens.
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);
  (void)data;

  std::shared_ptr<FileHandle> orphaned;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &orphaned).ok());
  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);

  // Model the metadata race result after a hard Link revived the inode.
  meta->prepare_reclaim_status = Status::NotFound("inode is still linked");
  ASSERT_TRUE(orphaned->Release().ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);

  std::shared_ptr<FileHandle> revived;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &revived).ok());
  ASSERT_TRUE(revived->Release().ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1) << "local orphan deferral must not stick after the last reference";
}

FIBER_TEST_F(FileHandleTest, ReclaimDataIsIdempotentWhenInodeAlreadyGone) {
  // The metadata engine reports the inode as no longer reclaimable (a
  // concurrent reclaim already finalized it). The coordinator treats that as
  // a no-op success: nothing to prepare, nothing to delete, nothing to
  // complete.
  ResetVolumeFromFiberForTest();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();
  meta->prepare_reclaim_status = Status::NotFound("inode is not reclaimable");

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(42).ok());
  EXPECT_EQ(meta->prepare_reclaim_calls, 1);
  EXPECT_EQ(meta->complete_reclaim_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty());
}

}  // namespace
}  // namespace swordfs::vfs
