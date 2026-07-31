// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandleManager — singleton fh → FileReadWriter mapping.

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

#include "vfs/FileHandleManager.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

class FileHandleManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    volume::VolumeImpl::Initialize();
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
    uint64_t fh;
    auto st = FileHandleManager::Instance().Open(ino, &fh);
    EXPECT_TRUE(st.ok()) << st.message();
    fhs_.push_back(fh);
    return fh;
  }

  std::vector<uint64_t> fhs_;
};

// ────────────────────────────────────────────────────────────────
// Singleton
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, InstanceIsSingleton) {
  auto &a = FileHandleManager::Instance();
  auto &b = FileHandleManager::Instance();
  EXPECT_EQ(&a, &b);
}

// ────────────────────────────────────────────────────────────────
// Open + Find
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, OpenAndFind) {
  uint64_t fh;
  auto st = FileHandleManager::Instance().Open(42, &fh);
  EXPECT_TRUE(st.ok()) << st.message();

  auto found = FileHandleManager::Instance().Find(fh);
  EXPECT_TRUE(found.has_value());
}

TEST_F(FileHandleManagerTest, FindNonexistent) {
  auto found = FileHandleManager::Instance().Find(999);
  EXPECT_FALSE(found.has_value());
}

TEST_F(FileHandleManagerTest, OpenMultipleHandles) {
  uint64_t fh1, fh2;
  EXPECT_TRUE(FileHandleManager::Instance().Open(10, &fh1).ok());
  EXPECT_TRUE(FileHandleManager::Instance().Open(20, &fh2).ok());
  EXPECT_NE(fh1, fh2);

  auto f1 = FileHandleManager::Instance().Find(fh1);
  auto f2 = FileHandleManager::Instance().Find(fh2);

  ASSERT_TRUE(f1.has_value());
  ASSERT_TRUE(f2.has_value());
  EXPECT_NE(f1->file_readwriter.get(), f2->file_readwriter.get());
}

// ────────────────────────────────────────────────────────────────
// Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, ReleaseRemovesHandle) {
  uint64_t fh;
  EXPECT_TRUE(FileHandleManager::Instance().Open(7, &fh).ok());

  FileHandleManager::Instance().Release(fh);
  EXPECT_FALSE(FileHandleManager::Instance().Find(fh).has_value());
}

TEST_F(FileHandleManagerTest, ReleaseNonexistentNoCrash) {
  // Releasing a handle that was never opened should not crash.
  FileHandleManager::Instance().Release(999);
  SUCCEED();
}

TEST_F(FileHandleManagerTest, ReleaseKeepsOtherHandles) {
  uint64_t fh1, fh2;
  EXPECT_TRUE(FileHandleManager::Instance().Open(1, &fh1).ok());
  EXPECT_TRUE(FileHandleManager::Instance().Open(2, &fh2).ok());

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
TEST_F(FileHandleManagerTest, OpenReturnsUniqueFh) {
  uint64_t fh1, fh2;
  EXPECT_TRUE(FileHandleManager::Instance().Open(100, &fh1).ok());
  EXPECT_TRUE(FileHandleManager::Instance().Open(200, &fh2).ok());
  EXPECT_NE(fh1, fh2);
}

// ────────────────────────────────────────────────────────────────
// Shared ownership — Find keeps handle alive across Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, FindKeepsHandleAliveAfterRelease) {
  uint64_t fh;
  EXPECT_TRUE(FileHandleManager::Instance().Open(55, &fh).ok());

  // Hold a shared_ptr before releasing.
  auto held = FileHandleManager::Instance().Find(fh);
  ASSERT_TRUE(held.has_value());

  FileHandleManager::Instance().Release(fh);
  // Map entry is gone.
  auto after = FileHandleManager::Instance().Find(fh);
  EXPECT_FALSE(after.has_value());
  ASSERT_TRUE(held.has_value());
  EXPECT_NE(held->file_readwriter.get(), nullptr);  // still alive
}

// ────────────────────────────────────────────────────────────────
// Concurrency — basic multi-threaded access
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, ConcurrentOpenAndFind) {
  constexpr int kThreads = 4;
  constexpr int kIters = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t] {
      for (int i = 0; i < kIters; ++i) {
        uint64_t fh;
        auto ino = static_cast<metadata::InodeID>(t * kIters + i + 100);
        EXPECT_TRUE(FileHandleManager::Instance()
                        .Open(ino, &fh)
                        .ok());
        auto found = FileHandleManager::Instance().Find(fh);
        EXPECT_TRUE(found.has_value());
      }
    });
  }
  for (auto &th : threads) th.join();

  // Clean up — release handles we know about (auto-allocated, consecutive
  // from 1).  The test threads opened kThreads * kIters handles.
  for (uint64_t fh = 1; fh <= kThreads * kIters; ++fh) {
    FileHandleManager::Instance().Release(fh);
  }
}

}  // namespace
}  // namespace swordfs::vfs
