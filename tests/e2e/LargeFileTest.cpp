// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: large file I/O across chunk boundaries.
//
// Validates: read/write spanning multiple chunks, seeking,
//            sparse files, and consistency of large data.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class LargeFileTest : public ::testing::Test {
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
// Cross-chunk writes
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, WriteCrossingChunkBoundary) {
  // Default chunk size is 64 MiB.  Write 80 MiB to cross at least one
  // chunk boundary.
  constexpr size_t kSize = 80ULL * 1024 * 1024;  // 80 MiB
  std::string data(kSize, 'A');
  ASSERT_TRUE(fixture_.WriteFile("big.bin", data));
  EXPECT_EQ(fixture_.ReadFile("big.bin"), data);
}

TEST_F(LargeFileTest, ReadPastEndOfFile) {
  ASSERT_TRUE(fixture_.WriteFile("small.bin", "hello"));
  // Reading past EOF should return only what's available.
  EXPECT_EQ(fixture_.ReadFile("small.bin"), "hello");
  auto st = fixture_.Stat("small.bin");
  EXPECT_EQ(st.st_size, 5);
}

// ────────────────────────────────────────────────────────────────
// Seek + write (sparse / hole-punching pattern)
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, SeekWriteHole) {
  std::string path = fixture_.MountPath("sparse.bin");
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);

  // Write at offset 0.
  const char* begin = "BEGIN";
  ssize_t n = ::pwrite(fd, begin, 5, 0);
  ASSERT_EQ(n, 5);

  // Write at offset 1 MiB.
  const char* end = "END";
  n = ::pwrite(fd, end, 3, 1024 * 1024);
  ASSERT_EQ(n, 3);

  ::close(fd);

  auto st = fixture_.Stat("sparse.bin");
  EXPECT_EQ(st.st_size, 1024 * 1024 + 3);

  // Read back the first bytes.
  std::string content = fixture_.ReadFile("sparse.bin");
  ASSERT_GE(content.size(), 5u);
  EXPECT_EQ(content.substr(0, 5), "BEGIN");
}

// ────────────────────────────────────────────────────────────────
// Multiple small writes accumulating to a chunk
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, ManySmallWrites) {
  constexpr int kNumWrites = 100;
  constexpr int kWriteSize = 4096;

  std::string path = fixture_.MountPath("many.bin");
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);

  std::string expected;
  for (int i = 0; i < kNumWrites; ++i) {
    std::string chunk(kWriteSize, static_cast<char>('A' + (i % 26)));
    ssize_t n = ::write(fd, chunk.data(), chunk.size());
    ASSERT_EQ(n, static_cast<ssize_t>(chunk.size()));
    expected += chunk;
  }
  ::close(fd);

  EXPECT_EQ(fixture_.ReadFile("many.bin"), expected);
}

// ────────────────────────────────────────────────────────────────
// Read at arbitrary offsets
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, ReadAtOffset) {
  std::string data(10000, 'X');
  ASSERT_TRUE(fixture_.WriteFile("offset.bin", data));

  std::string path = fixture_.MountPath("offset.bin");
  int fd = ::open(path.c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);

  char buf[100];
  ssize_t n = ::pread(fd, buf, sizeof(buf), 5000);
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), std::string(100, 'X'));

  ::close(fd);
}

// ────────────────────────────────────────────────────────────────
// Fsync
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, FsyncAfterWrite) {
  std::string path = fixture_.MountPath("fsync.bin");
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);

  const char* data = "fsync test data";
  ssize_t n = ::write(fd, data, std::strlen(data));
  ASSERT_GT(n, 0);

  EXPECT_EQ(::fsync(fd), 0);
  ::close(fd);

  EXPECT_EQ(fixture_.ReadFile("fsync.bin"), "fsync test data");
}

// ────────────────────────────────────────────────────────────────
// Flush on close
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, DataVisibleAfterClose) {
  std::string path = fixture_.MountPath("close.bin");
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::write(fd, "before close", 12), 12);
  ::close(fd);

  EXPECT_EQ(fixture_.ReadFile("close.bin"), "before close");
}
