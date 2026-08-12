// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: large file I/O across chunk boundaries.
//
// Validates: read/write spanning multiple chunks, seeking,
//            sparse files, and consistency of large data.

#include <fcntl.h>
#include <gtest/gtest.h>
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
  const char *name = "big.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, data), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, data.size(), Fixture::Hash64(data)));
}

TEST_F(LargeFileTest, ReadPastEndOfFile) {
  ASSERT_EQ(fixture_.CreateFile("small.bin", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("small.bin", "hello"), 0);
  // Reading past EOF should return only what's available.
  EXPECT_TRUE(fixture_.FileEquals("small.bin", 5, Fixture::Hash64("hello")));
  struct stat st;
  ASSERT_EQ(fixture_.Stat("small.bin", &st), 0);
  EXPECT_EQ(st.st_size, 5);
}

// ────────────────────────────────────────────────────────────────
// Seek + write (sparse / hole-punching pattern)
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, SeekWriteHole) {
  const char *name_sparse = "sparse.bin";
  ASSERT_EQ(fixture_.CreateFile(name_sparse, 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile(name_sparse, O_RDWR);
  ASSERT_GE(fd, 0);

  // Write at offset 0.
  const char *begin = "BEGIN";
  ssize_t n = ::pwrite(fd, begin, 5, 0);
  ASSERT_EQ(n, 5);

  // Write at offset 1 MiB.
  const char *end = "END";
  n = ::pwrite(fd, end, 3, 1024 * 1024);
  ASSERT_EQ(n, 3);

  ::close(fd);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name_sparse, &st), 0);
  EXPECT_EQ(st.st_size, 1024 * 1024 + 3);

  // Read back the first bytes.
  std::string content;
  ASSERT_EQ(fixture_.ReadFile(name_sparse, &content), 0);
  ASSERT_GE(content.size(), 5u);
  EXPECT_EQ(content.substr(0, 5), "BEGIN");
}

// ────────────────────────────────────────────────────────────────
// Multiple small writes accumulating to a chunk
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, ManySmallWrites) {
  constexpr int kNumWrites = 100;
  constexpr int kWriteSize = 4096;

  const char *name_many = "many.bin";
  ASSERT_EQ(fixture_.CreateFile(name_many, 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile(name_many, O_RDWR);
  ASSERT_GE(fd, 0);

  std::string expected;
  for (int i = 0; i < kNumWrites; ++i) {
    std::string chunk(kWriteSize, static_cast<char>('A' + (i % 26)));
    ssize_t n = ::write(fd, chunk.data(), chunk.size());
    ASSERT_EQ(n, static_cast<ssize_t>(chunk.size()));
    expected += chunk;
  }
  ::close(fd);

  EXPECT_TRUE(fixture_.FileEquals(name_many, expected.size(), Fixture::Hash64(expected)));
}

// ────────────────────────────────────────────────────────────────
// Read at arbitrary offsets
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, ReadAtOffset) {
  std::string data(10000, 'X');
  const char *name_off = "offset.bin";
  ASSERT_EQ(fixture_.CreateFile(name_off, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name_off, data), 0);

  int fd = fixture_.OpenFile(name_off, O_RDONLY);
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
  const char *name_fsync = "fsync.bin";
  ASSERT_EQ(fixture_.CreateFile(name_fsync, 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile(name_fsync, O_RDWR);
  ASSERT_GE(fd, 0);

  const char *data = "fsync test data";
  ssize_t n = ::write(fd, data, std::strlen(data));
  ASSERT_GT(n, 0);

  EXPECT_EQ(::fsync(fd), 0);
  ::close(fd);

  EXPECT_TRUE(fixture_.FileEquals(name_fsync, 15, Fixture::Hash64("fsync test data")));
}

// ────────────────────────────────────────────────────────────────
// Flush on close
// ────────────────────────────────────────────────────────────────

TEST_F(LargeFileTest, DataVisibleAfterClose) {
  ASSERT_EQ(fixture_.CreateFile("close.bin", 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile("close.bin", O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::write(fd, "before close", 12), 12);
  ::close(fd);

  EXPECT_TRUE(fixture_.FileEquals("close.bin", 12, Fixture::Hash64("before close")));
}
