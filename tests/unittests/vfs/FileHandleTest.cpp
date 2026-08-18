// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandle — the fh → FileHandle mapping and the
// high-level Open/Create entry points.

#include <folly/fibers/FiberManagerInternal.h>
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
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::ChunkMeta;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::utils::Status;

// Minimal IMetaEngine — every op succeeds; Create fabricates an inode.
class MockMetaEngine : public IMetaEngine {
 public:
  Status Lookup(InodeID, std::string_view, InodeID *,
                struct stat *) override { return Status::OK(); }
  Status GetAttr(InodeID, struct stat *) override { return Status::OK(); }
  Status ReadDir(InodeID,
                 std::vector<swordfs::metadata::SwordFsEntry> *) override {
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, mode_t, InodeID *child_ino,
                struct stat *attr) override {
    *child_ino = next_ino_++;
    std::memset(attr, 0, sizeof(*attr));
    attr->st_ino = *child_ino;
    attr->st_mode = S_IFREG | 0644;
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID *,
               struct stat *) override { return Status::OK(); }
  Status Unlink(InodeID, std::string_view) override { return Status::OK(); }
  Status RmDir(InodeID, std::string_view) override { return Status::OK(); }
  Status Rename(InodeID, std::string_view, InodeID,
                std::string_view, unsigned int) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const struct stat *, int,
                 struct stat *) override { return Status::OK(); }
  Status StatFs(struct statvfs *) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Symlink(InodeID, std::string_view, const char *, InodeID *,
                 struct stat *) override { return Status::OK(); }
  Status Link(InodeID, InodeID, std::string_view,
              struct stat *) override { return Status::OK(); }
  Status Readlink(InodeID, std::string *) override { return Status::OK(); }
  Status Open(InodeID) override { return open_status; }
  Status ReclaimInode(InodeID) override {
    ++reclaim_calls;
    return reclaim_status;
  }
  Status ListChunks(InodeID, std::vector<ChunkMeta> *) override {
    return Status::OK();
  }
  Status OpenDir(InodeID) override { return Status::OK(); }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }
  Status AddChunk(InodeID, const ChunkMeta &) override { return Status::OK(); }
  Status FindChunk(InodeID, ChunkIndex, ChunkMeta *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, size_t size) override {
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
    auto meta = std::make_unique<MockMetaEngine>();
    mock_meta_ = meta.get();
    volume::VolumeImpl::Instance().set_meta_engine(std::move(meta));
  }

  void TearDown() override {
    // Clean up any handles left by a test.
    for (uint64_t fh : fhs_) {
      FileHandleManager::Instance().Release(fh);
    }
    volume::VolumeImpl::Initialize();
  }

  /// Helper: open a handle and remember it for cleanup.
  uint64_t OpenHandle(metadata::InodeID ino) {
    FileHandle handle;
    auto status = FileHandle::Open(ino, 0, &handle);
    EXPECT_TRUE(status.ok());
    uint64_t fh = handle.fh();
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
  auto &a = FileHandleManager::Instance();
  auto &b = FileHandleManager::Instance();
  EXPECT_EQ(&a, &b);
}

// ────────────────────────────────────────────────────────────────
// Open + Find
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, OpenAndFind) {
  uint64_t fh = OpenHandle(42);

  auto found = FileHandleManager::Instance().Find(fh);
  EXPECT_TRUE(found.has_value());
}

TEST_F(FileHandleTest, FindNonexistent) {
  auto found = FileHandleManager::Instance().Find(999);
  EXPECT_FALSE(found.has_value());
}

TEST_F(FileHandleTest, OpenMultipleHandles) {
  uint64_t fh1 = OpenHandle(10);
  uint64_t fh2 = OpenHandle(20);
  EXPECT_NE(fh1, fh2);

  auto f1 = FileHandleManager::Instance().Find(fh1);
  auto f2 = FileHandleManager::Instance().Find(fh2);

  ASSERT_TRUE(f1.has_value());
  ASSERT_TRUE(f2.has_value());
  EXPECT_NE(f1->handle().get(), f2->handle().get());
}

// ────────────────────────────────────────────────────────────────
// Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, ReleaseRemovesHandle) {
  uint64_t fh = OpenHandle(7);

  FileHandleManager::Instance().Release(fh);
  EXPECT_FALSE(FileHandleManager::Instance().Find(fh).has_value());
}

TEST_F(FileHandleTest, ReleaseNonexistentNoCrash) {
  // Releasing a handle that was never opened should not crash.
  FileHandleManager::Instance().Release(999);
  SUCCEED();
}

TEST_F(FileHandleTest, ReleaseKeepsOtherHandles) {
  uint64_t fh1 = OpenHandle(1);
  uint64_t fh2 = OpenHandle(2);

  FileHandleManager::Instance().Release(fh1);

  EXPECT_FALSE(FileHandleManager::Instance().Find(fh1).has_value());
  auto f2 = FileHandleManager::Instance().Find(fh2);
  ASSERT_TRUE(f2.has_value());
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
// Shared ownership — Find keeps handle alive across Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, FindKeepsHandleAliveAfterRelease) {
  uint64_t fh = OpenHandle(55);

  // Hold a shared_ptr before releasing.
  auto held = FileHandleManager::Instance().Find(fh);
  ASSERT_TRUE(held.has_value());

  FileHandleManager::Instance().Release(fh);
  // Map entry is gone.
  auto after = FileHandleManager::Instance().Find(fh);
  EXPECT_FALSE(after.has_value());
  ASSERT_TRUE(held.has_value());
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
        FileHandle handle;
        auto status = FileHandle::Open(ino, 0, &handle);
        EXPECT_TRUE(status.ok());
        auto found = FileHandleManager::Instance().Find(handle.fh());
        EXPECT_TRUE(found.has_value());
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
}

// ────────────────────────────────────────────────────────────────
// OpenDir / ReleaseDir
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, OpenDirAllocatesHandle) {
  auto &mgr = FileHandleManager::Instance();
  uint64_t dh = mgr.OpenDir(42);
  EXPECT_GT(dh, 0u);
  mgr.ReleaseDir(dh);
}

TEST_F(FileHandleTest, OpenDirUniqueHandles) {
  auto &mgr = FileHandleManager::Instance();
  uint64_t dh1 = mgr.OpenDir(10);
  uint64_t dh2 = mgr.OpenDir(20);
  EXPECT_NE(dh1, dh2);
  mgr.ReleaseDir(dh1);
  mgr.ReleaseDir(dh2);
}

TEST_F(FileHandleTest, ReleaseDirNonexistentNoCrash) {
  // Releasing a directory handle that was never allocated should not crash.
  FileHandleManager::Instance().ReleaseDir(999);
  SUCCEED();
}

TEST_F(FileHandleTest, OpenDirAndReleaseDirLifecycle) {
  auto &mgr = FileHandleManager::Instance();
  constexpr metadata::InodeID kIno = 77;

  uint64_t dh = mgr.OpenDir(kIno);
  EXPECT_NE(dh, 0u);

  // Release and then re-allocate — should get a new handle.
  mgr.ReleaseDir(dh);
  uint64_t dh2 = mgr.OpenDir(kIno);
  EXPECT_NE(dh, dh2);
  mgr.ReleaseDir(dh2);
}

// ────────────────────────────────────────────────────────────────
// FileHandle::Open error propagation
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, OpenMetaFailurePropagates) {
  mock_meta_->open_status = Status::Permission("denied");
  FileHandle handle;
  auto status = FileHandle::Open(42, 0, &handle);
  EXPECT_TRUE(status.IsPermission());
}

TEST_F(FileHandleTest, OpenTruncateAppliesOTrunc) {
  FileHandle handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  ASSERT_TRUE(status.ok());
  fhs_.push_back(handle.fh());
  EXPECT_EQ(mock_meta_->truncate_calls, 1);
  EXPECT_EQ(mock_meta_->last_truncate_size, 0u);
}

TEST_F(FileHandleTest, OpenTruncateFailurePropagates) {
  mock_meta_->truncate_status = Status::Internal("truncate failed");
  FileHandle handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), Status::kInternal);
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleTest, InodeHandleGetMissingWithoutCreate) {
  EXPECT_EQ(
      InodeHandleManager::Instance().Get(9001, /*create_if_missing=*/false),
      nullptr);
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
  FileHandle handle;
  ASSERT_TRUE(FileHandle::Open(9004, 0, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9004, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_TRUE(inode_handle->MarkOrphanedIfOpen());

  FileHandleManager::Instance().Release(handle.fh());
  EXPECT_EQ(mock_meta_->reclaim_calls, 1);
}

TEST_F(FileHandleTest, CloseOnlyReclaimsOnLastReference) {
  FileHandle h1, h2;
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h1).ok());
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h2).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9005, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_TRUE(inode_handle->MarkOrphanedIfOpen());

  // First close: open count 2 → 1 — no reclaim yet.
  FileHandleManager::Instance().Release(h1.fh());
  EXPECT_EQ(mock_meta_->reclaim_calls, 0);
  EXPECT_EQ(inode_handle->open_count(), 1);

  // Final close: open count 1 → 0 — reclaim now.
  FileHandleManager::Instance().Release(h2.fh());
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
// InodeHandleManager::HasOpenHandles
// ────────────────────────────────────────────────────────────────
//
// `VfsImpl::Unlink` consults `InodeHandleManager::HasOpenHandles` to
// decide whether to defer the inode reclaim. The case below exercises
// the manager's lifecycle: no entry → open fd → entry present →
// close → entry absent.

TEST_F(FileHandleTest, InodeHandleManagerHasOpenHandles) {
  // No handle yet -> manager reports no open fds.
  InodeID test_ino = 9998;
  EXPECT_FALSE(InodeHandleManager::Instance().HasOpenHandles(test_ino));

  // FileHandle::Open routes through mock_meta_ and creates the handle.
  FileHandle handle;
  ASSERT_TRUE(FileHandle::Open(test_ino, O_RDWR, &handle).ok());
  EXPECT_TRUE(InodeHandleManager::Instance().HasOpenHandles(test_ino));

  // Release drops the entry.
  FileHandleManager::Instance().Release(handle.fh());
  EXPECT_FALSE(InodeHandleManager::Instance().HasOpenHandles(test_ino));
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager::ReclaimData — coordinator contract
// ────────────────────────────────────────────────────────────────
//
// The manager is responsible for fanning a single ReclaimData call out
// to both the metadata engine (ListChunks + ReclaimInode) and the data
// engine (Delete per chunk key). These tests assert that the call
// sequence matches the documented contract; the per-engine behaviour
// is exercised by the metadata- and data-engine unit tests.

namespace {

// Mock data engine: records every Delete call and lets the test inject
// per-key failure responses.
class FakeDataEngine : public swordfs::storage::IDataEngine {
 public:
  swordfs::storage::DataEngineLimits Limits() const override {
    return {};
  }
  bool Head(std::string_view, size_t *) override { return false; }
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

// Mock metadata engine: records ListChunks / ReclaimInode invocations
// and returns a configurable chunk list.
class TrackingMetaEngine final : public swordfs::metadata::IMetaEngine {
 public:
  Status Lookup(InodeID, std::string_view, InodeID *,
                struct stat *) override { return Status::OK(); }
  Status GetAttr(InodeID, struct stat *) override { return Status::OK(); }
  Status ReadDir(InodeID,
                 std::vector<swordfs::metadata::SwordFsEntry> *) override {
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, mode_t, InodeID *,
                struct stat *) override { return Status::OK(); }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID *,
               struct stat *) override { return Status::OK(); }
  Status Unlink(InodeID, std::string_view) override { return Status::OK(); }
  Status RmDir(InodeID, std::string_view) override { return Status::OK(); }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view,
                unsigned int) override { return Status::OK(); }
  Status SetAttr(InodeID, const struct stat *, int,
                 struct stat *) override { return Status::OK(); }
  Status StatFs(struct statvfs *) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Symlink(InodeID, std::string_view, const char *, InodeID *,
                 struct stat *) override { return Status::OK(); }
  Status Link(InodeID, InodeID, std::string_view,
              struct stat *) override { return Status::OK(); }
  Status Readlink(InodeID, std::string *) override { return Status::OK(); }
  Status Open(InodeID) override { return Status::OK(); }
  Status ReclaimInode(InodeID ino) override {
    ++reclaim_inode_calls;
    last_reclaim_ino = ino;
    return Status::OK();
  }
  Status ListChunks(InodeID ino,
                    std::vector<swordfs::metadata::ChunkMeta> *out) override {
    ++list_chunks_calls;
    last_list_ino = ino;
    if (!list_chunks_status.ok()) {
      return list_chunks_status;
    }
    *out = chunks;
    return Status::OK();
  }
  Status OpenDir(InodeID) override { return Status::OK(); }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }
  Status AddChunk(InodeID, const swordfs::metadata::ChunkMeta &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, swordfs::metadata::ChunkIndex,
                   swordfs::metadata::ChunkMeta *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, size_t) override { return Status::OK(); }

  int list_chunks_calls = 0;
  int reclaim_inode_calls = 0;
  InodeID last_list_ino = 0;
  InodeID last_reclaim_ino = 0;
  Status list_chunks_status = Status::OK();
  std::vector<swordfs::metadata::ChunkMeta> chunks;
};

}  // namespace

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
  swordfs::metadata::ChunkMeta c0{};
  c0.index = 0;
  c0.start_offset = 0;
  c0.key = "4242/0";
  c0.size = 1024;
  swordfs::metadata::ChunkMeta c1{};
  c1.index = 1;
  c1.start_offset = 65536;
  c1.key = "4242/1";
  c1.size = 2048;
  meta->chunks = {c0, c1};

  // Install engines in the volume singleton so ReclaimData can find them.
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(
      meta_up.release()));
  vol.set_data_engine(
      std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  // Drive the coordinator.
  ASSERT_TRUE(InodeHandleManager::ReclaimData(4242).ok());

  // Order is ascending chunk index (ListChunks contract).
  EXPECT_EQ(data->delete_calls.size(), 2);
  EXPECT_EQ(data->delete_calls[0], "4242/0");
  EXPECT_EQ(data->delete_calls[1], "4242/1");

  // Both engines received exactly one call each.
  EXPECT_EQ(meta->list_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
  EXPECT_EQ(meta->last_list_ino, 4242);
  EXPECT_EQ(meta->last_reclaim_ino, 4242);
}

TEST_F(FileHandleTest, ReclaimDataWithNoDataEngineStillDropsInode) {
  // If no data engine is configured (e.g. format-only mount), the
  // coordinator must still drop the metadata side. This guards against
  // the previous bug where a missing data engine left chunk metadata
  // orphaned.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto *meta = meta_up.get();

  swordfs::metadata::ChunkMeta c{};
  c.index = 0;
  c.key = "99/0";
  meta->chunks = {c};

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(
      meta_up.release()));
  // No data engine set.

  ASSERT_TRUE(InodeHandleManager::ReclaimData(99).ok());
  EXPECT_EQ(meta->list_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
}

TEST_F(FileHandleTest, ReclaimDataCallsReclaimInodeEvenWhenChunkEmpty) {
  // Inode with zero registered chunks: the manager must still invoke
  // ReclaimInode so the inode is dropped from the metadata engine.
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto *meta = meta_up.get();

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(
      meta_up.release()));
  // No data engine — the manager must tolerate this and proceed.

  ASSERT_TRUE(InodeHandleManager::ReclaimData(123).ok());
  EXPECT_EQ(meta->list_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
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

  swordfs::metadata::ChunkMeta c0{};
  c0.index = 0;
  c0.key = "1/0";
  swordfs::metadata::ChunkMeta c1{};
  c1.index = 1;
  c1.key = "1/1";
  meta->chunks = {c0, c1};
  data->fail_keys["1/0"] = Status::Internal("forced");

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(
      meta_up.release()));
  vol.set_data_engine(
      std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  ASSERT_TRUE(InodeHandleManager::ReclaimData(1).ok());
  // Both chunks were attempted despite the failure on "1/0".
  EXPECT_EQ(data->delete_calls.size(), 2);
  EXPECT_EQ(meta->reclaim_inode_calls, 1);
}

TEST_F(FileHandleTest, ReclaimDataPropagatesListChunksFailure) {
  // If the metadata engine refuses to enumerate, the manager must
  // surface that error and NOT invoke ReclaimInode (we don't know
  // whether the inode exists from the manager's perspective).
  swordfs::volume::VolumeImpl::Initialize();
  auto meta_up = std::make_unique<TrackingMetaEngine>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();
  meta->list_chunks_status = Status::Internal("nope");

  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine>(
      meta_up.release()));
  vol.set_data_engine(
      std::unique_ptr<swordfs::storage::IDataEngine>(data_up.release()));

  auto st = InodeHandleManager::ReclaimData(42);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(meta->list_chunks_calls, 1);
  EXPECT_EQ(meta->reclaim_inode_calls, 0);
  EXPECT_TRUE(data->delete_calls.empty());
}

}  // namespace
}  // namespace swordfs::vfs
