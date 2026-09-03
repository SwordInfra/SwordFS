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

TEST_F(FileIOTest, MultipleWritesAccumulate) {
  const std::string name = "multi.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);

  ASSERT_EQ(::write(fd, "hello", 5), 5);
  ASSERT_EQ(::write(fd, " ", 1), 1);
  ASSERT_EQ(::write(fd, "world", 5), 5);
  ::close(fd);

  EXPECT_TRUE(fixture_.FileEquals(name, 11, Fixture::Hash64("hello world")));
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 11);
}

TEST_F(FileIOTest, Overwrite) {
  const std::string name = "overwrite.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);

  ASSERT_EQ(::write(fd, "hello", 5), 5);
  // Seek back to start and overwrite.
  ASSERT_EQ(::lseek(fd, 0, SEEK_SET), 0);
  ASSERT_EQ(::write(fd, "HELLO", 5), 5);
  ::close(fd);

  EXPECT_TRUE(fixture_.FileEquals(name, 5, Fixture::Hash64("HELLO")));
}

TEST_F(FileIOTest, GapWrite) {
  const std::string name = "gap.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR), 0);
  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);

  ASSERT_EQ(::write(fd, "AAA", 3), 3);
  // Seek forward, leaving a hole from offset 3 to 99.
  ASSERT_EQ(::lseek(fd, 100, SEEK_SET), 100);
  ASSERT_EQ(::write(fd, "BBB", 3), 3);
  ::close(fd);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 103);

  // Verify the three segments: "AAA", hole of zeros, "BBB".
  std::string content;
  ASSERT_EQ(fixture_.ReadFile(name, &content), 0);
  ASSERT_EQ(content.size(), 103u);
  EXPECT_EQ(content.substr(0, 3), "AAA");
  EXPECT_EQ(content.substr(3, 97), std::string(97, '\0'));
  EXPECT_EQ(content.substr(100, 3), "BBB");
}

// ────────────────────────────────────────────────────────────────
// Append
// ────────────────────────────────────────────────────────────────

TEST_F(FileIOTest, ReopenAndWriteFails) {
  const std::string name = "reopen.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);

  // First write + close triggers chunk flush (→ kFlushed).
  ASSERT_EQ(fixture_.WriteFile(name, "hello"), 0);

  // Re-opening for write should fail because the flushed chunk is
  // not writable.
  int fd = fixture_.OpenFile(name, O_WRONLY);
  ASSERT_GE(fd, 0);
  const std::string extra = " world";
  ssize_t n = ::write(fd, extra.c_str(), extra.size());
  EXPECT_EQ(n, -1) << "expected write to fail on flushed chunk, got n=" << n;
  EXPECT_EQ(errno, EINVAL) << "expected EINVAL";
  ::close(fd);

  // Unchanged: still only "hello".
  EXPECT_TRUE(fixture_.FileEquals(name, 5, Fixture::Hash64("hello")));
}

// FIXME: O_SYNC/O_DSYNC write-through is not yet implemented.
// O_SYNC requires every write() to be durable before it returns, i.e. the
// touched chunk must be flushed on each write.  That is blocked by the same
// overwrite-after-flush limitation as ReopenAndWriteFails: a flushed chunk
// cannot accept further writes (no JuiceFS-style slice merge yet), so the
// second write below would fail with EINVAL.  O_DIRECT (bypass the chunk
// buffer) is a related gap.  Enable this test once slice/overwrite
// semantics land and the O_SYNC/O_DSYNC/O_DIRECT paths are wired through
// FileHandle → InodeHandle → FileReadWriter.
TEST_F(FileIOTest, DISABLED_OSyncWriteIsDurable) {
  const std::string name = "osync.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  int fd = fixture_.OpenFile(name, O_WRONLY | O_SYNC);
  ASSERT_GE(fd, 0);

  // Each write must be durable on return — no explicit fsync needed.
  ASSERT_EQ(::write(fd, "hello", 5), 5);
  ASSERT_EQ(::write(fd, " world", 6), 6);
  ::close(fd);

  EXPECT_TRUE(fixture_.FileEquals(name, 11, Fixture::Hash64("hello world")));
}

// FIXME: Cross-open append is not yet supported.
// When a file is closed and reopened with O_APPEND, the dirty chunk
// from the first open is flushed and removed; the second open creates
// a fresh FileReadWriter whose WriteBuf only covers the newly written
// range, losing the prefix data in the flushed chunk.
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

  EXPECT_TRUE(fixture_.FileEquals(name, 11, Fixture::Hash64("hello world")));
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

  EXPECT_TRUE(fixture_.FileEquals(name, 11, Fixture::Hash64("hello world")));
}

// ────────────────────────────────────────────────────────────────
// Read
// ────────────────────────────────────────────────────────────────

TEST_F(FileIOTest, CreateWriteRead) {
  const std::string name = "hello.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "Hello, SwordFS!"), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, 15, Fixture::Hash64("Hello, SwordFS!")));
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

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(FileIOTest, TruncateShrink) {
  const std::string name = "t.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hello world"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 5), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, 5, Fixture::Hash64("hello")));
}

TEST_F(FileIOTest, TruncateExtend) {
  const std::string name = "t.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hi"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 10), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 10);
}

TEST_F(FileIOTest, TruncateShrinkThenExtendClearsStaleChunkData) {
  const std::string name = "truncate_stale.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hello world"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 5), 0);
  ASSERT_EQ(fixture_.Truncate(name, 11), 0);

  std::string content;
  ASSERT_EQ(fixture_.ReadFile(name, &content), 0);
  ASSERT_EQ(content.size(), 11U);
  EXPECT_EQ(content.substr(0, 5), "hello");
  EXPECT_EQ(content.substr(5), std::string(6, '\0'));
}

// ────────────────────────────────────────────────────────────────
// Open-unlink semantics: unlink while fd open, then close
// ────────────────────────────────────────────────────────────────
//
// POSIX guarantees that a file that has been unlinked can still be
// read/written through a pre-existing fd, and that its data only goes
// away once the last fd is closed. After this refactor the metadata
// engine defers inode deletion via the runtime `OpenHandleTracker`
// (see `OpenHandleTracker.hpp`); this end-to-end test verifies the
// contract from the filesystem-client perspective.

TEST_F(FileIOTest, UnlinkWhileOpenKeepsFileReadable) {
  const std::string name = "open_unlink.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "payload"), 0);
  ASSERT_EQ(::close(fixture_.OpenFile(name, O_RDONLY)), 0);

  int fd = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd, 0);

  // unlink while fd is still open.
  ASSERT_EQ(fixture_.UnlinkFile(name), 0);

  // fd must still read the original contents.
  char buf[16] = {};
  ssize_t n = ::pread(fd, buf, sizeof(buf), 0);
  ASSERT_EQ(n, 7);
  EXPECT_STREQ(buf, "payload");
  ASSERT_EQ(::close(fd), 0);

  // After the last close, the name is gone AND the inode is gone: a
  // fresh lookup returns ENOENT.
  struct stat st;
  EXPECT_EQ(fixture_.Stat(name, &st), -1);
}
