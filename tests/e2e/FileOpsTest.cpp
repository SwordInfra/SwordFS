// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: regular file operations.
//
// Validates: creat, write, read, unlink, stat, statfs, truncate,
//            append, overwrite, file name length limits,
//            rename name length limit.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class FileOpsTest : public ::testing::Test {
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
// Create / write / read / unlink
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, CreateWriteReadUnlink) {
  ASSERT_EQ(fixture_.WriteFile("hello.txt", "Hello, SwordFS!"), 0);
  EXPECT_TRUE(fixture_.CheckFile("hello.txt", "Hello, SwordFS!"));

  ASSERT_EQ(::unlink(fixture_.MountPath("hello.txt").c_str()), 0);
  struct stat st;
  EXPECT_NE(::stat(fixture_.MountPath("hello.txt").c_str(), &st), 0);
}

TEST_F(FileOpsTest, WriteEmptyFile) {
  ASSERT_EQ(fixture_.WriteFile("empty.txt", ""), 0);
  EXPECT_TRUE(fixture_.CheckFile("empty.txt", ""));

  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("empty.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_size, 0);
}

TEST_F(FileOpsTest, WriteLargeData) {
  std::string data(1024 * 1024, 'X');
  ASSERT_EQ(fixture_.WriteFile("large.bin", data), 0);
  EXPECT_TRUE(fixture_.CheckFile("large.bin", data));
}

TEST_F(FileOpsTest, AppendToFile) {
  ASSERT_EQ(fixture_.WriteFile("append.txt", "hello"), 0);

  std::string path = fixture_.MountPath("append.txt");
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  const char* extra = " world";
  ssize_t n = ::write(fd, extra, 6);
  ::close(fd);
  ASSERT_EQ(n, 6);

  EXPECT_TRUE(fixture_.CheckFile("append.txt", "hello world"));
}

TEST_F(FileOpsTest, UnlinkNonexistent) {
  ASSERT_EQ(::unlink(fixture_.MountPath("no_such_file").c_str()), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, FileInRootIsFile) {
  ASSERT_EQ(fixture_.WriteFile("plain.txt", "text"), 0);
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("plain.txt").c_str(), &st), 0);
  EXPECT_FALSE(S_ISDIR(st.st_mode));
  EXPECT_TRUE(S_ISREG(st.st_mode));
}

// ────────────────────────────────────────────────────────────────
// stat / statvfs
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, StatRoot) {
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("").c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_GT(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(st.st_nlink, static_cast<nlink_t>(2));
}

TEST_F(FileOpsTest, StatFile) {
  ASSERT_EQ(fixture_.WriteFile("s.txt", "data"), 0);
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("s.txt").c_str(), &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 4);
}

TEST_F(FileOpsTest, StatfsReturnsValidData) {
  auto sv = fixture_.Statfs();
  EXPECT_GT(sv.f_bsize, static_cast<unsigned long>(0));
  EXPECT_GT(sv.f_blocks, static_cast<fsblkcnt_t>(0));
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, TruncateShrink) {
  ASSERT_EQ(fixture_.WriteFile("t.txt", "hello world"), 0);
  ASSERT_EQ(::truncate(fixture_.MountPath("t.txt").c_str(), 5), 0);
  EXPECT_TRUE(fixture_.CheckFile("t.txt", "hello"));
}

TEST_F(FileOpsTest, TruncateExtend) {
  ASSERT_EQ(fixture_.WriteFile("t.txt", "hi"), 0);
  ASSERT_EQ(::truncate(fixture_.MountPath("t.txt").c_str(), 10), 0);
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("t.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_size, 10);
}

// ────────────────────────────────────────────────────────────────
// Name length limits
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, CreateFileAtLimit) {
  std::string name(255, 'x');
  ASSERT_EQ(fixture_.WriteFile(name, "ok"), 0);
  EXPECT_TRUE(fixture_.CheckFile(name, "ok"));
}

TEST_F(FileOpsTest, CreateFileNameTooLong) {
  std::string name(256, 'x');
  std::string path = fixture_.MountPath(name);
  int fd = ::creat(path.c_str(), 0644);
  ASSERT_EQ(fd, -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
}

TEST_F(FileOpsTest, RenameTargetNameTooLong) {
  ASSERT_EQ(fixture_.WriteFile("src.txt", "data"), 0);
  std::string long_name(256, 'x');
  ASSERT_EQ(::rename(fixture_.MountPath("src.txt").c_str(),
                         fixture_.MountPath(long_name).c_str()), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
  EXPECT_TRUE(fixture_.CheckFile("src.txt", "data"));
}
