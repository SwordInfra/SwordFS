// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: file I/O operations.
//
// Validates: write, read, append.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class FileIOTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp()) << "Failed to set up E2E fixture";
  }
  void TearDown() override {
    fixture_.TearDown();
  }
  Fixture fixture_;
};

// ────────────────────────────────────────────────────────────────
// Write
// ────────────────────────────────────────────────────────────────

TEST_F(FileIOTest, WriteEmptyFile) {
  const std::string name = "empty.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, ""), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, 0, Fixture::Hash64("")));

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 0);
}

TEST_F(FileIOTest, WriteLargeData) {
  const std::string name = "large.bin";
  std::string data(1024 * 1024, 'X');
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, data), 0);
  EXPECT_TRUE(
      fixture_.FileEquals(name, data.size(), Fixture::Hash64(data)));
}

// ────────────────────────────────────────────────────────────────
// Append
// ────────────────────────────────────────────────────────────────

// FIXME(juicefs-slice): Cross-open append is not yet supported.
// When a file is closed and reopened with O_APPEND, the dirty chunk
// from the first open is flushed and removed; the second open creates
// a fresh FileReadWriter whose WriteBuf only covers the newly written
// range, losing the prefix data in the flushed chunk.  A proper slice
// mechanism (à la JuiceFS) is needed to stitch partial chunks.
TEST_F(FileIOTest, DISABLED_AppendToFile) {
  const std::string name = "append.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hello"), 0);

  int fd = fixture_.OpenFile(name, O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  const std::string extra = " world";
  ssize_t n = ::write(fd, extra.c_str(), extra.size());
  ::close(fd);
  ASSERT_EQ(n, 6);

  EXPECT_TRUE(
      fixture_.FileEquals(name, 11, Fixture::Hash64("hello world")));
}

TEST_F(FileIOTest, AppendMultipleWithinSameOpen) {
  const std::string name = "append_same.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);

  // Open once with O_APPEND, then write multiple times without closing.
  int fd = fixture_.OpenFile(name, O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);

  ASSERT_EQ(::write(fd, "hello", 5), 5);
  ASSERT_EQ(::write(fd, " ", 1), 1);
  ASSERT_EQ(::write(fd, "world", 5), 5);
  ::close(fd);

  EXPECT_TRUE(
      fixture_.FileEquals(name, 11, Fixture::Hash64("hello world")));
}

// ────────────────────────────────────────────────────────────────
// Read
// ────────────────────────────────────────────────────────────────

TEST_F(FileIOTest, CreateWriteRead) {
  const std::string name = "hello.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "Hello, SwordFS!"), 0);
  EXPECT_TRUE(
      fixture_.FileEquals(name, 15, Fixture::Hash64("Hello, SwordFS!")));
}

TEST_F(FileIOTest, FileInRootIsFile) {
  const std::string name = "plain.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "text"), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_FALSE(S_ISDIR(st.st_mode));
  EXPECT_TRUE(S_ISREG(st.st_mode));
}
