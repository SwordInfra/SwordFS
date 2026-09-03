// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandle — the fh → FileHandle mapping and the
// high-level Open/Create entry points.

#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

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
    // Default to nlink==0 so ReclaimData's final guard proceeds unless a
    // test installs an engine with specific inode metadata.
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
  Status Unlink(InodeID, std::string_view, uint64_t *) override {
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
  Status ReclaimInode(InodeID) override {
    ++reclaim_calls;
    return reclaim_status;
  }
  Status VisitChunks(InodeID, const swordfs::metadata::ChunkVisitorFn &) override {
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status AddChunk(InodeID, const SwordFsChunk &) override {
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
  int reclaim_calls = 0;
  Status open_status = Status::OK();
  Status truncate_status = Status::OK();
  Status reclaim_status = Status::OK();

 private:
  InodeID next_ino_ = 1000;
};

class FileHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    volume::VolumeImpl::Initialize();
    // Drop any per-inode state left by a prior test. The
    // InodeHandleManager is a process-lifetime singleton; without this
    // reset, leaked InodeHandles (especially with non-zero open_count)
    // would make Get(ino, false) hand out stale handles and ReclaimData's
    // last-line-of-defence guard refuse on arbitrary later inodes. The
    // same Initialize() is also called on the production mount path.
    InodeHandleManager::Instance().Initialize();
    auto meta = std::make_unique<MockMetaEngine>();
    mock_meta_ = meta.get();
    volume::VolumeImpl::Instance().set_meta_engine(std::move(meta));
    // ReclaimData now requires a data engine (production invariant —
    // --bucket is required). Tests that exercise the orphan-close
    // reclaim path (e.g. CloseReclaimsOrphanedInode) would otherwise
    // trip the CHECK(), so install a no-op fake here.
    volume::VolumeImpl::Instance().set_data_engine(std::make_unique<NoopDataEngine>());
  }

  void TearDown() override {
    // Clean up any handles left by a test.
    for (uint64_t fh : fhs_) {
      if (auto handle = HandleManager::Instance().FindAs<FileHandle>(fh)) {
        handle->Release();
      } else if (auto handle = HandleManager::Instance().FindAs<DirHandle>(fh)) {
        handle->Release();
      }
    }
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

TEST_F(FileHandleTest, InstanceIsSingleton) {
  auto &a = HandleManager::Instance();
  auto &b = HandleManager::Instance();
  EXPECT_EQ(&a, &b);
}

// ────────────────────────────────────────────────────────────────
// Open + Find
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, OpenAndFind) {
  uint64_t fh = OpenHandle(42);

  auto found = HandleManager::Instance().FindAs<FileHandle>(fh);
  EXPECT_NE(found, nullptr);
}

TEST_F(FileHandleTest, FindNonexistent) {
  auto found = HandleManager::Instance().FindAs<FileHandle>(999);
  EXPECT_EQ(found, nullptr);
}

TEST_F(FileHandleTest, OpenMultipleHandles) {
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

TEST_F(FileHandleTest, ReleaseRemovesHandle) {
  uint64_t fh = OpenHandle(7);
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  ASSERT_NE(handle, nullptr);
  ASSERT_TRUE(handle->Release().ok());

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh), nullptr);
}

TEST_F(FileHandleTest, ReleaseKeepsOtherHandles) {
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
TEST_F(FileHandleTest, OpenReturnsUniqueFh) {
  uint64_t fh1 = OpenHandle(100);
  uint64_t fh2 = OpenHandle(200);
  EXPECT_NE(fh1, fh2);
}

// ────────────────────────────────────────────────────────────────
// Shared ownership — Find keeps handle alive across Unregister
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, FindKeepsHandleAliveAfterUnregister) {
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

TEST_F(FileHandleTest, ConcurrentOpenAndFind) {
  constexpr int kThreads = 4;
  constexpr int kIters = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t] {
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
    });
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

TEST_F(FileHandleTest, RegisterAndFindDirectoryHandle) {
  auto handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t fh = HandleManager::Instance().Register(handle);

  auto found = HandleManager::Instance().FindAs<DirHandle>(fh);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found, handle);
  ASSERT_TRUE(handle->Release().ok());
}

TEST_F(FileHandleTest, RegisterAssignsUniqueHandlesAcrossTypes) {
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

TEST_F(FileHandleTest, FindAsRejectsWrongHandleType) {
  auto handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t fh = HandleManager::Instance().Register(handle);

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh), nullptr);
  EXPECT_NE(HandleManager::Instance().FindAs<DirHandle>(fh), nullptr);
  ASSERT_TRUE(handle->Release().ok());
}

// ────────────────────────────────────────────────────────────────
// FileHandle::Open error propagation
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, OpenMetaFailurePropagates) {
  mock_meta_->open_status = Status::Permission("denied");
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, 0, &handle);
  EXPECT_TRUE(status.IsPermission());
}

TEST_F(FileHandleTest, OpenTruncateAppliesOTrunc) {
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  ASSERT_TRUE(status.ok());
  fhs_.push_back(handle->fh());
  EXPECT_EQ(mock_meta_->truncate_calls, 1);
  EXPECT_EQ(mock_meta_->last_truncate_size, 0u);
}

TEST_F(FileHandleTest, OpenTruncateFailurePropagates) {
  mock_meta_->truncate_status = Status::Internal("truncate failed");
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), Status::kInternal);
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, InodeHandleGetMissingWithoutCreate) {
  EXPECT_EQ(InodeHandleManager::Instance().Get(9001, /*create_if_missing=*/false), nullptr);
}

TEST_F(FileHandleTest, InodeHandleGetExistingTracksOpenCount) {
  uint64_t fh = OpenHandle(9002);
  auto inode_handle = InodeHandleManager::Instance().Get(9002, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->ino(), 9002);
  EXPECT_EQ(inode_handle->open_count(), 1);
}

TEST_F(FileHandleTest, InodeHandleRecreatedAfterExpiry) {
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

TEST_F(FileHandleTest, CloseReclaimsOrphanedInode) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(9004, 0, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9004, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_TRUE(inode_handle->MarkOrphanedIfOpen());

  ASSERT_TRUE(handle->Release().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);
}

TEST_F(FileHandleTest, CloseOnlyReclaimsOnLastReference) {
  std::shared_ptr<FileHandle> h1, h2;
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h1).ok());
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h2).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9005, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_TRUE(inode_handle->MarkOrphanedIfOpen());

  // First close: open count 2 → 1 — no reclaim yet.
  ASSERT_TRUE(h1->Release().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 0);
  EXPECT_EQ(inode_handle->open_count(), 1);

  // Final close: open count 1 → 0 — reclaim now.
  ASSERT_TRUE(h2->Release().ok());
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);
  EXPECT_EQ(inode_handle->open_count(), 0);
}

TEST_F(FileHandleTest, MarkOrphanedIfOpenFalseWhenNoOpenFds) {
  auto inode_handle = InodeHandleManager::Instance().Get(9006, true);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_FALSE(inode_handle->MarkOrphanedIfOpen());
  EXPECT_EQ(inode_handle->open_count(), 0);
}

// ────────────────────────────────────────────────────────────────
// InodeHandle open-fd tracking (VfsImpl::Unlink decision)
// ────────────────────────────────────────────────────────────────
//
// `VfsImpl::Unlink` consults the per-inode InodeHandle (via
// InodeHandleManager::Get) to decide whether to defer the inode
// reclaim. The case below exercises the lifecycle: no handle → open fd
// → handle present with open_count>0 → close → open_count back to 0.

TEST_F(FileHandleTest, InodeHandleOpenFdTracking) {
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
// ReclaimData fans a single call out to both the metadata engine
// (VisitChunks + ReclaimInode) and the data engine (Delete per chunk
// key). These tests assert that the call sequence matches the
// documented contract; the per-engine behaviour is exercised by the
// metadata- and data-engine unit tests.

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
    return Status::OK();
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
  std::vector<std::string> delete_calls;
  std::unordered_map<std::string, Status> fail_keys;
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
  Status Unlink(InodeID, std::string_view, uint64_t *) override {
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
    return Status::OK();
  }
  Status ReclaimInode(InodeID ino) override {
    ++reclaim_inode_calls;
    last_reclaim_ino = ino;
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
  Status AddChunk(InodeID, const swordfs::metadata::SwordFsChunk &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, swordfs::metadata::ChunkIndex, swordfs::metadata::SwordFsChunk *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, uint64_t) override {
    return Status::OK();
  }

  int visit_chunks_calls = 0;
  int reclaim_inode_calls = 0;
  InodeID last_visit_ino = 0;
  InodeID last_reclaim_ino = 0;
  Status visit_chunks_status = Status::OK();
  std::vector<swordfs::metadata::SwordFsChunk> chunks;
  std::unordered_map<InodeID, struct stat> attrs;
};

}  // namespace

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

TEST_F(FileHandleTest, ReclaimDataDeletesEveryChunkAndCallsReclaimInode) {
  // Ensure the singleton exists before we touch it — the fixture's
  // SetUp() normally does this, but a test run via --gtest_filter may
  // bypass it.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  // Keep raw pointers around for assertions after the engines move into
  // the volume singleton (which takes ownership).
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  // Seed two chunk records.
  swordfs::metadata::SwordFsChunk c0{};
  c0.index = 0;
  c0.start_offset = 0;
  c0.key = "4242/0";
  c0.size = 1024;
  swordfs::metadata::SwordFsChunk c1{};
  c1.index = 1;
  c1.start_offset = 65536;
  c1.key = "4242/1";
  c1.size = 2048;
  meta->chunks = {c0, c1};
  // Register the inode so ReclaimData's nlink guard sees nlink==0 and
  // proceeds with the chunk enumeration.
  struct stat attr{};
  attr.st_nlink = 0;
  meta->SetAttr(4242, attr);

  // Install engines in the volume singleton so ReclaimData can find them.
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(4242).ok());

  // The mock visits chunks in insertion order.
  EXPECT_EQ(data->delete_calls.size(), 2);
  EXPECT_EQ(data->delete_calls[0], "4242/0");
  EXPECT_EQ(data->delete_calls[1], "4242/1");

  // Both engines received exactly one call each.
  EXPECT_EQ(meta->visit_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
  EXPECT_EQ(meta->last_visit_ino, 4242);
  EXPECT_EQ(meta->last_reclaim_ino, 4242);
}

TEST_F(FileHandleTest, ReclaimDataDeletesChunkObjectsViaDataEngine) {
  // Counterpart of ReclaimDataWithNoDataEngineStillDropsInode from the
  // pre-CHECK() era. The data engine is now a hard requirement on the
  // ReclaimData path (the production mount always installs one via
  // `--bucket`); exercising the chunk-delete loop with a real (fake)
  // data engine covers the same "delete every chunk + drop inode"
  // contract that test used to assert.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  swordfs::metadata::SwordFsChunk c{};
  c.index = 0;
  c.key = "99/0";
  meta->chunks = {c};
  struct stat attr{};
  attr.st_nlink = 0;
  meta->SetAttr(99, attr);

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(99).ok());
  EXPECT_EQ(data->delete_calls.size(), 1u);
  EXPECT_EQ(data->delete_calls[0], "99/0");
  EXPECT_EQ(meta->visit_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
}

TEST_F(FileHandleTest, ReclaimDataCallsReclaimInodeEvenWhenChunkEmpty) {
  // Inode with zero registered chunks: the manager must still invoke
  // ReclaimInode so the inode is dropped from the metadata engine.
  // A data engine is still required (see CHECK in ReclaimData) — even
  // though no Delete call will be issued, the engine must be installed.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  struct stat attr{};
  attr.st_nlink = 0;
  meta->SetAttr(123, attr);

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(123).ok());
  EXPECT_EQ(meta->visit_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
  EXPECT_TRUE(data->delete_calls.empty());
}

TEST_F(FileHandleTest, ReclaimDataContinuesAfterPerChunkFailure) {
  // A failing per-chunk Delete must not stop the cleanup: every chunk
  // gets attempted and ReclaimInode still runs. A stranded object is
  // GC's job; the metadata view must converge regardless.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  swordfs::metadata::SwordFsChunk c0{};
  c0.index = 0;
  c0.key = "1/0";
  swordfs::metadata::SwordFsChunk c1{};
  c1.index = 1;
  c1.key = "1/1";
  meta->chunks = {c0, c1};
  data->fail_keys["1/0"] = Status::Internal("forced");
  struct stat attr{};
  attr.st_nlink = 0;
  meta->SetAttr(1, attr);

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(1).ok());
  // Both chunks were attempted despite the failure on "1/0".
  EXPECT_EQ(data->delete_calls.size(), 2);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
}

TEST_F(FileHandleTest, ReclaimDataPropagatesVisitChunksFailure) {
  // If the metadata engine refuses to enumerate, the manager must
  // surface that error and NOT invoke ReclaimInode (we don't know
  // whether the inode exists from the manager's perspective).
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();
  meta->visit_chunks_status = Status::Internal("nope");
  struct stat attr{};
  attr.st_nlink = 0;
  meta->SetAttr(42, attr);

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  auto st = ReclaimInode(42);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(meta->visit_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty());
}

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
  return {meta, data};
}

}  // namespace

TEST_F(FileHandleTest, ReclaimDataRefusesWhenNlinkStillPositive) {
  // A caller (VfsImpl::Unlink racing a Link) decided nlink==0 and
  // called ReclaimData, but a Link landed between its check and
  // ours. The point of no return is here — refuse to drop the chunk
  // objects from under the surviving hardlink name.
  swordfs::volume::VolumeImpl::Initialize();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/2);
  // Add a chunk so we'd have something to delete if the guard fell
  // through; the absence of any delete is what we actually verify.
  swordfs::metadata::SwordFsChunk chunk{};
  chunk.index = 0;
  chunk.key = "7/0";
  meta->chunks = {chunk};

  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_TRUE(data->delete_calls.empty()) << "ReclaimData must NOT delete chunk objects when a hardlink "
                                             "still references the inode";
  EXPECT_EQ(meta->reclaim_inode_calls, 0) << "ReclaimData must NOT drop the inode while nlink > 0";
}

TEST_F(FileHandleTest, ReclaimDataRefusesWhileAnOpenHandleHoldsTheInode) {
  // Caller forgot to route through MarkOrphaned — refuse so the
  // open fd's reads continue to work. The data engine must see no
  // Delete calls.
  swordfs::volume::VolumeImpl::Initialize();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);

  // Open a file handle on this inode. The fixture's MockMetaEngine
  // lets everything through, so this just installs a tracked fh.
  std::shared_ptr<FileHandle> fh;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &fh).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(7, false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_GT(inode_handle->open_count(), 0u);

  ASSERT_TRUE(ReclaimInode(7).ok());
  EXPECT_TRUE(data->delete_calls.empty()) << "ReclaimData must NOT delete chunk objects while a fd is open";
  EXPECT_EQ(meta->reclaim_inode_calls, 0);

  // Cleanup
  ASSERT_TRUE(fh->Release().ok());
}

TEST_F(FileHandleTest, ReclaimDataIsIdempotentWhenInodeAlreadyGone) {
  // The metadata engine reports the inode as gone (NotFound on GetAttr).
  // The InodeHandle::ReclaimData guard treats that as a no-op success:
  // a concurrent reclaim has already finalised the cleanup on another
  // thread. Without this guard, the manager would still call VisitChunks
  // and surface a NotFound to the caller as if it were a real failure.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();
  // Deliberately do NOT register any attrs for ino 42 — GetAttr
  // returns NotFound.

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(meta_up.release()));
  vol.set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(ReclaimInode(42).ok());
  EXPECT_EQ(meta->visit_chunks_calls, 0);
  EXPECT_EQ(meta->reclaim_inode_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty());
}

}  // namespace
}  // namespace swordfs::vfs
