// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandleManager — singleton fh → FileReadWriter mapping.

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

#include "vfs/FileHandleManager.hpp"
#include "vfs/FileReadWriter.hpp"

namespace swordfs::vfs {
namespace {

class FileHandleManagerTest : public ::testing::Test {
   protected:
  void TearDown() override {
    // Clean up any handles left by a test.
    FileHandleManager::Instance().Release(1);
    FileHandleManager::Instance().Release(2);
    FileHandleManager::Instance().Release(42);
  }
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
  auto rw = std::make_unique<FileReadWriter>(nullptr, nullptr, 42);
  FileHandleManager::Instance().Open(1, std::move(rw));

  auto found = FileHandleManager::Instance().Find(1);
  EXPECT_NE(found, nullptr);
}

TEST_F(FileHandleManagerTest, FindNonexistent) {
  auto found = FileHandleManager::Instance().Find(999);
  EXPECT_EQ(found, nullptr);
}

TEST_F(FileHandleManagerTest, OpenMultipleHandles) {
  auto rw1 = std::make_unique<FileReadWriter>(nullptr, nullptr, 10);
  auto rw2 = std::make_unique<FileReadWriter>(nullptr, nullptr, 20);

  FileHandleManager::Instance().Open(1, std::move(rw1));
  FileHandleManager::Instance().Open(2, std::move(rw2));

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
  auto rw = std::make_unique<FileReadWriter>(nullptr, nullptr, 7);
  FileHandleManager::Instance().Open(1, std::move(rw));

  FileHandleManager::Instance().Release(1);
  EXPECT_EQ(FileHandleManager::Instance().Find(1), nullptr);
}

TEST_F(FileHandleManagerTest, ReleaseNonexistentNoCrash) {
  // Releasing a handle that was never opened should not crash.
  FileHandleManager::Instance().Release(999);
  SUCCEED();
}

TEST_F(FileHandleManagerTest, ReleaseKeepsOtherHandles) {
  auto rw1 = std::make_unique<FileReadWriter>(nullptr, nullptr, 1);
  auto rw2 = std::make_unique<FileReadWriter>(nullptr, nullptr, 2);

  FileHandleManager::Instance().Open(1, std::move(rw1));
  FileHandleManager::Instance().Open(2, std::move(rw2));

  FileHandleManager::Instance().Release(1);

  EXPECT_EQ(FileHandleManager::Instance().Find(1), nullptr);
  auto f2 = FileHandleManager::Instance().Find(2);
  ASSERT_NE(f2, nullptr);
}

// ────────────────────────────────────────────────────────────────
// Open overwrite
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, OpenOverwritesExistingHandle) {
  auto rw1 = std::make_unique<FileReadWriter>(nullptr, nullptr, 100);
  FileHandleManager::Instance().Open(1, std::move(rw1));

  auto rw2 = std::make_unique<FileReadWriter>(nullptr, nullptr, 200);
  FileHandleManager::Instance().Open(1, std::move(rw2));

  auto found = FileHandleManager::Instance().Find(1);
  ASSERT_NE(found, nullptr);
  // Overwritten handle was replaced — the old unique_ptr is gone.
}

// ────────────────────────────────────────────────────────────────
// Shared ownership — Find keeps handle alive across Release
// ────────────────────────────────────────────────────────────────

TEST_F(FileHandleManagerTest, FindKeepsHandleAliveAfterRelease) {
  auto rw = std::make_unique<FileReadWriter>(nullptr, nullptr, 55);
  FileHandleManager::Instance().Open(1, std::move(rw));

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
    threads.emplace_back([t] {
      for (int i = 0; i < kIters; ++i) {
        uint64_t fh = static_cast<uint64_t>(t) * kIters + i + 100;
        auto rw = std::make_unique<FileReadWriter>(
            nullptr, nullptr, static_cast<metadata::InodeID>(fh));
        FileHandleManager::Instance().Open(fh, std::move(rw));
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
