// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: regular file operations.
//
// Validates: create, open, unlink, stat, statfs, truncate, rename (name length).

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>

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
// Open
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, OpenExistingFile) {
  const std::string name = "f.txt";
  const std::string content = "hello";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(name, &before), 0);
  ASSERT_EQ(before.st_size, content.size());

  int fd = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd, 0);

  struct stat after;
  ASSERT_EQ(fixture_.Stat(name, &after), 0);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_EQ(after.st_size, before.st_size);

  ::close(fd);
}

TEST_F(FileOpsTest, OpenNonexistent) {
  int fd = fixture_.OpenFile("noent", O_RDONLY);
  ASSERT_EQ(fd, -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, OpenDirectoryFails) {
  const std::string name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);

  // Opening a directory with O_RDWR or O_WRONLY → EISDIR.
  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_EQ(fd, -1);
  EXPECT_EQ(errno, EISDIR);
}

TEST_F(FileOpsTest, OpenMultipleHandles) {
  // Multiple independent handles to the same file should coexist.
  const std::string name = "multi.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);

  int fd1 = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd1, 0);
  int fd2 = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd2, 0);
  ASSERT_NE(fd1, fd2);

  ::close(fd1);
  ::close(fd2);
}

TEST_F(FileOpsTest, OpenUnlinkStillReadable) {
  // After unlink, open fd should still be readable until closed.
  const std::string name = "unlink_me.txt";
  const std::string content = "hello";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);

  int fd = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd, 0);

  ASSERT_EQ(fixture_.UnlinkFile(name), 0);
  ASSERT_EQ(fixture_.OpenFile(name, O_RDONLY), -1);

  // File entry is gone, but fd still works.
  char *buf = new char[content.size() + 1];
  ASSERT_EQ(::read(fd, buf, content.size()), content.size());
  buf[content.size()] = '\0';
  EXPECT_STREQ(buf, content.c_str());

  ::close(fd);
  delete[] buf;
}

// ────────────────────────────────────────────────────────────────
// Create
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, CreateRegularFile) {
  struct stat st;
  // ASCII name
  const std::string ascii = "hello.txt";
  ASSERT_EQ(fixture_.CreateFile(ascii, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.Stat(ascii, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 0);
  EXPECT_EQ(st.st_nlink, 1);
  EXPECT_TRUE(fixture_.UmaskEquals(ascii, 0644));

  // UTF-8 name (Chinese characters)
  const std::string utf8 = "你好世界.txt";
  ASSERT_EQ(fixture_.CreateFile(utf8, 0600, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.Stat(utf8, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 0);
  EXPECT_EQ(st.st_nlink, 1);
  EXPECT_TRUE(fixture_.UmaskEquals(utf8, 0600));
}

TEST_F(FileOpsTest, CreateAlreadyExists) {
  const std::string name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_EXCL), -1);
  EXPECT_EQ(errno, EEXIST);
}

TEST_F(FileOpsTest, CreateWithoutExclTruncates) {
  // Without O_EXCL, recreating the same file truncates and succeeds.
  const std::string name = "t.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "original data"), 0);
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 0);
  EXPECT_TRUE(fixture_.UmaskEquals(name, 0644));
}

TEST_F(FileOpsTest, CreateWithoutTruncPreserves) {
  // Without O_TRUNC, recreating the same file preserves content.
  const std::string name = "keep.txt";
  const std::string content = "original data";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, content.size());
  EXPECT_TRUE(fixture_.UmaskEquals(name, 0644));
}

TEST_F(FileOpsTest, CreateNameAtLimit) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length, 'x');
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
}

TEST_F(FileOpsTest, CreateNameTooLong) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length + 1, 'x');
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
}

TEST_F(FileOpsTest, CreateOverDirectory) {
  // Creating a regular file where a directory already exists → EISDIR.
  const std::string name = "mydir";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, EISDIR);
}

TEST_F(FileOpsTest, CreateUnderNonexistent) {
  // Parent directory does not exist → ENOENT.
  ASSERT_EQ(fixture_.CreateFile("noent/file.txt", 0644, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, CreatePathComponentNotDir) {
  // A path component is a regular file, not a directory → ENOTDIR.
  const std::string name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.CreateFile(name + "/sub", 0644, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

// ────────────────────────────────────────────────────────────────
// Unlink
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, UnlinkSuccess) {
  const std::string name = "hello.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.UnlinkFile(name), 0);
  struct stat st;
  EXPECT_NE(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, UnlinkNonexistent) {
  ASSERT_EQ(fixture_.UnlinkFile("no_such_file"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, UnlinkDirectoryFails) {
  const std::string name = "mydir";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.UnlinkFile(name), -1);
  EXPECT_EQ(errno, EISDIR);
}

// ────────────────────────────────────────────────────────────────
// stat / statvfs
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, StatRoot) {
  struct stat st;
  ASSERT_EQ(fixture_.Stat("", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_GT(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(st.st_nlink, static_cast<nlink_t>(2));
}

TEST_F(FileOpsTest, StatFile) {
  const std::string name = "s.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 4);
}

TEST_F(FileOpsTest, StatfsReturnsValidData) {
  auto limits = fixture_.GetLimits();
  struct statvfs sv{};
  ASSERT_EQ(fixture_.Statfs(&sv), 0);
  EXPECT_GT(sv.f_bsize, static_cast<unsigned long>(0));
  EXPECT_GT(sv.f_frsize, static_cast<unsigned long>(0));
  EXPECT_GE(sv.f_blocks, static_cast<fsblkcnt_t>(0));
  EXPECT_GE(sv.f_bfree, static_cast<fsblkcnt_t>(0));
  EXPECT_GE(sv.f_bavail, static_cast<fsblkcnt_t>(0));
  EXPECT_GT(sv.f_files, static_cast<fsfilcnt_t>(0));
  EXPECT_GE(sv.f_ffree, static_cast<fsfilcnt_t>(0));
  EXPECT_GT(sv.f_namemax, static_cast<unsigned long>(0));
  EXPECT_LE(sv.f_namemax, static_cast<unsigned long>(limits.max_name_length));
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, TruncateShrink) {
  const std::string name = "t.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hello world"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 5), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, 5, Fixture::Hash64("hello")));
}

TEST_F(FileOpsTest, TruncateExtend) {
  const std::string name = "t.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hi"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 10), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 10);
}

// ────────────────────────────────────────────────────────────────
// Rename (name length)
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, RenameTargetNameTooLong) {
  const std::string name = "src.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  std::string long_name(256, 'x');
  ASSERT_EQ(fixture_.Rename(name, long_name),
            -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
  EXPECT_TRUE(fixture_.FileEquals(name, 4, Fixture::Hash64("data")));
}

// ────────────────────────────────────────────────────────────────
// Hardlink
// ────────────────────────────────────────────────────────────────

// ────────────────────────────────────────────────────────────────
// Symbolic link
// ────────────────────────────────────────────────────────────────