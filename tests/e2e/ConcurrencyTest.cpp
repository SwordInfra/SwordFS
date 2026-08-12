// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: concurrent filesystem access.
//
// Validates: concurrent reads, concurrent writes to different files,
//            concurrent mkdir, and basic thread safety.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class ConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp());
  }
  void TearDown() override {
    fixture_.TearDown();
  }
  Fixture fixture_;
};

// ────────────────────────────────────────────────────────────────
// Concurrent reads
// ────────────────────────────────────────────────────────────────

TEST_F(ConcurrencyTest, ConcurrentReads) {
  const char *name = "shared.bin";
  std::string data(65536, 'R');
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, data), 0);

  std::atomic<int> errors{0};
  auto reader = [&](int id) {
    for (int i = 0; i < 10; ++i) {
      if (!fixture_.FileEquals(name, data.size(), Fixture::Hash64(data))) {
        errors.fetch_add(1);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(reader, i);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);
}

// ────────────────────────────────────────────────────────────────
// Concurrent writes to different files
// ────────────────────────────────────────────────────────────────

TEST_F(ConcurrencyTest, ConcurrentWritesDifferentFiles) {
  constexpr int kNumThreads = 4;
  std::atomic<int> errors{0};

  auto writer = [&](int id) {
    for (int i = 0; i < 5; ++i) {
      std::string fname = "file_" + std::to_string(id) + "_" + std::to_string(i);
      std::string content = "thread_" + std::to_string(id) + "_iter_" + std::to_string(i);
      if (fixture_.CreateFile(fname, 0644, O_CREAT | O_WRONLY | O_TRUNC) != 0) {
        errors.fetch_add(1);
        continue;
      }
      if (fixture_.WriteFile(fname, content) != 0) {
        errors.fetch_add(1);
        continue;
      }
      if (!fixture_.FileEquals(fname, content.size(), Fixture::Hash64(content))) {
        errors.fetch_add(1);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back(writer, i);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);

  // Verify all files exist.
  std::vector<std::string> entries;
  fixture_.ReadDir(".", &entries);
  EXPECT_EQ(entries.size(), static_cast<size_t>(kNumThreads * 5));
}

// ────────────────────────────────────────────────────────────────
// Concurrent mkdir
// ────────────────────────────────────────────────────────────────

TEST_F(ConcurrencyTest, ConcurrentMkdir) {
  constexpr int kNumDirs = 20;
  std::atomic<int> created{0};

  auto maker = [&](int start, int end) {
    for (int i = start; i < end; ++i) {
      std::string dirname = "dir_" + std::to_string(i);
      if (fixture_.MkDir(dirname, 0755) == 0) {
        created.fetch_add(1);
      }
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(maker, 0, 10);
  threads.emplace_back(maker, 10, 20);
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(created.load(), kNumDirs);
  std::vector<std::string> entries;
  fixture_.ReadDir(".", &entries);
  EXPECT_EQ(entries.size(), static_cast<size_t>(kNumDirs));
}

// ────────────────────────────────────────────────────────────────
// Concurrent stat (no modification, just reads)
// ────────────────────────────────────────────────────────────────

TEST_F(ConcurrencyTest, ConcurrentStat) {
  const char *name = "st.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "stat me"), 0);
  std::atomic<int> errors{0};

  auto stater = [&]() {
    for (int i = 0; i < 100; ++i) {
      struct stat st;
      if (fixture_.Stat(name, &st) != 0 ||
          !S_ISREG(st.st_mode) || st.st_size != 7) {
        errors.fetch_add(1);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(stater);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0);
}

// ────────────────────────────────────────────────────────────────
// Concurrent create + unlink
// ────────────────────────────────────────────────────────────────

TEST_F(ConcurrencyTest, ConcurrentCreateAndUnlink) {
  constexpr int kNumFiles = 30;
  std::atomic<int> created{0};
  std::atomic<int> unlinked{0};

  auto creator = [&]() {
    for (int i = 0; i < kNumFiles; ++i) {
      std::string fname = "temp_" + std::to_string(i);
      int fd = fixture_.CreateFile(fname, 0644, O_CREAT | O_WRONLY | O_TRUNC);
      if (fd >= 0) {
        ::close(fd);
        created.fetch_add(1);
      }
    }
  };

  auto unlinker = [&]() {
    for (int i = 0; i < kNumFiles; ++i) {
      std::string fname = "temp_" + std::to_string(i);
      if (fixture_.UnlinkFile(fname) == 0) {
        unlinked.fetch_add(1);
      }
    }
  };

  std::thread t1(creator);
  std::thread t2(unlinker);
  t1.join();
  t2.join();

  // At least all files were created once.
  EXPECT_EQ(created.load(), kNumFiles);
}
