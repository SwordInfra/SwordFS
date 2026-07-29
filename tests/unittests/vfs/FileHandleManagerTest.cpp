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
    vol_ = std::make_unique<volume::VolumeImpl>();
  }

  void TearDown() override {
    // Clean up any handles left by a test.
    FileHandleManager::Instance().Release(1);
    FileHandleManager::Instance().Release(2);
    FileHandleManager::Instance().Release(42);
  }

  std::unique_ptr<volume::VolumeImpl> vol_;
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
  auto st = FileHandleManager::Instance().Open(1, vol_.get(), 42);
  EXPECT_TRUE(st.ok()) << st.message();

  auto found = FileHandleManager::Instance().Find(1);
  EXPECT_NE(found, nullptr);
}

TEST_F(FileHandleManagerTest, FindNonexistent) {
  auto found = FileHandleManager::Instance().Find(999);
  EXPECT_EQ(found, nullptr);
}

TEST_F(FileHandleManagerTest, OpenMultipleHandles) {
  EXPECT_TRUE(FileHandleManager::Instance().Open(1, vol_.get(), 10).ok());
  EXPECT_TRUE(FileHandleManager::Instance().Open(2, vol_.get(), 20).ok());

  auto f1 = FileHandleManager::Instance().Find(1);
  auto f2 = FileHandleManager::Instance().Find(2);

  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);
  EXPECT_NE(f1.get(), f2.get());
}

// ────────────────────────────────────────────────────────────────
// Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, ReleaseRemovesHandle) {
  EXPECT_TRUE(FileHandleManager::Instance().Open(1, vol_.get(), 7).ok());

  FileHandleManager::Instance().Release(1);
  EXPECT_EQ(FileHandleManager::Instance().Find(1), nullptr);
}

TEST_F(FileHandleManagerTest, ReleaseNonexistentNoCrash) {
  // Releasing a handle that was never opened should not crash.
  FileHandleManager::Instance().Release(999);
  SUCCEED();
}

TEST_F(FileHandleManagerTest, ReleaseKeepsOtherHandles) {
  EXPECT_TRUE(FileHandleManager::Instance().Open(1, vol_.get(), 1).ok());
  EXPECT_TRUE(FileHandleManager::Instance().Open(2, vol_.get(), 2).ok());

  FileHandleManager::Instance().Release(1);

  EXPECT_EQ(FileHandleManager::Instance().Find(1), nullptr);
  auto f2 = FileHandleManager::Instance().Find(2);
  ASSERT_NE(f2, nullptr);
}

// ────────────────────────────────────────────────────────────────
// Open duplicate fh
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, OpenDuplicateFails) {
  EXPECT_TRUE(FileHandleManager::Instance().Open(1, vol_.get(), 100).ok());
  auto st = FileHandleManager::Instance().Open(1, vol_.get(), 200);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), utils::Status::Code::kAlreadyExists);

  // First handle is still intact.
  auto found = FileHandleManager::Instance().Find(1);
  ASSERT_NE(found, nullptr);
}

// ────────────────────────────────────────────────────────────────
// Shared ownership — Find keeps handle alive across Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, FindKeepsHandleAliveAfterRelease) {
  EXPECT_TRUE(FileHandleManager::Instance().Open(1, vol_.get(), 55).ok());

  // Hold a shared_ptr before releasing.
  auto held = FileHandleManager::Instance().Find(1);
  ASSERT_NE(held, nullptr);

  FileHandleManager::Instance().Release(1);
  // Map entry is gone, but our shared_ptr still owns the object.
  EXPECT_EQ(FileHandleManager::Instance().Find(1), nullptr);
  ASSERT_NE(held, nullptr);
  EXPECT_NE(held.get(), nullptr);  // still alive
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
        uint64_t fh = static_cast<uint64_t>(t) * kIters + i + 100;
        EXPECT_TRUE(FileHandleManager::Instance()
                        .Open(fh, vol_.get(),
                              static_cast<metadata::InodeID>(fh))
                        .ok());
        auto found = FileHandleManager::Instance().Find(fh);
        EXPECT_NE(found, nullptr);
      }
    });
  }
  for (auto &th : threads) th.join();

  // Clean up.
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kIters; ++i) {
      FileHandleManager::Instance().Release(
          static_cast<uint64_t>(t) * kIters + i + 100);
    }
  }
}

}  // namespace
}  // namespace swordfs::vfs
