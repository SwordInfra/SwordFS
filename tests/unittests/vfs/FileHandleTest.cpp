// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandle — the fh → FileHandle mapping and the
// high-level Open/Create entry points.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
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
  Status Open(InodeID) override { return Status::OK(); }
  Status ReclaimData(InodeID) override { return Status::OK(); }
  Status OpenDir(InodeID) override { return Status::OK(); }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }
  Status AddChunk(InodeID, const ChunkMeta &) override { return Status::OK(); }
  Status FindChunk(InodeID, ChunkIndex, ChunkMeta *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, size_t) override { return Status::OK(); }

 private:
  InodeID next_ino_ = 1000;
};

class FileHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    volume::VolumeImpl::Initialize();
    volume::VolumeImpl::Instance().set_meta_engine(
        std::make_unique<MockMetaEngine>());
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

}  // namespace
}  // namespace swordfs::vfs
