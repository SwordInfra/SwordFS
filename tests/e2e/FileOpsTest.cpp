// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: regular file operations.
//
// Validates: create, open, unlink, stat, statfs, file rename.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include "tests/e2e/Const.hpp"
#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;
using namespace swordfs::e2e;  // kDefaultDirMode, kDefaultFileMode,
                               // kDefaultCreateFlags

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
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
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

TEST_F(FileOpsTest, OpenMultipleHandles) {
  // Multiple independent handles to the same file should coexist.
  const std::string name = "multi.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);

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
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
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
  ASSERT_EQ(fixture_.CreateFile(ascii, kDefaultFileMode, kDefaultCreateFlags), 0);
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
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY | O_EXCL), -1);
  EXPECT_EQ(errno, EEXIST);
}

TEST_F(FileOpsTest, CreateWithoutExclTruncates) {
  // Without O_EXCL, recreating the same file truncates and succeeds.
  const std::string name = "t.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 0);
  EXPECT_TRUE(fixture_.UmaskEquals(name, 0644));
}

TEST_F(FileOpsTest, CreateWithoutTruncPreserves) {
  // Without O_TRUNC, recreating the same file preserves content.
  const std::string name = "keep.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, content.size());
  EXPECT_TRUE(fixture_.UmaskEquals(name, 0644));
}

TEST_F(FileOpsTest, CreateNameAtLimit) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length, 'x');
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
}

TEST_F(FileOpsTest, CreateNameTooLong) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length + 1, 'x');
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
}

TEST_F(FileOpsTest, CreateOverDirectory) {
  // Creating a regular file where a directory already exists → EISDIR.
  const std::string name = "mydir";
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, EISDIR);
}

TEST_F(FileOpsTest, CreateUnderNonexistent) {
  // Parent directory does not exist → ENOENT.
  ASSERT_EQ(fixture_.CreateFile("noent/file.txt", kDefaultFileMode, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, CreatePathComponentNotDir) {
  // A path component is a regular file, not a directory → ENOTDIR.
  const std::string name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.CreateFile(name + "/sub", kDefaultFileMode, O_CREAT | O_WRONLY), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

// ────────────────────────────────────────────────────────────────
// Unlink
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, UnlinkSuccess) {
  const std::string name = "hello.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.UnlinkFile(name), 0);
  struct stat st;
  EXPECT_NE(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, UnlinkNonexistent) {
  ASSERT_EQ(fixture_.UnlinkFile("no_such_file"), -1);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// stat / statvfs
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, StatRoot) {
  struct stat st;
  ASSERT_EQ(fixture_.Stat(".", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(swordfs::metadata::kRootInodeId));
  EXPECT_EQ(st.st_nlink, static_cast<nlink_t>(2));
}

TEST_F(FileOpsTest, StatFile) {
  const std::string name = "s.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_TRUE(fixture_.UmaskEquals(name, 0644));
  EXPECT_TRUE(st.st_nlink == static_cast<nlink_t>(1));
  EXPECT_EQ(st.st_size, static_cast<off_t>(content.size()));
}

TEST_F(FileOpsTest, StatNonexistent) {
  struct stat st;
  ASSERT_EQ(fixture_.Stat("noent", &st), -1);
  EXPECT_EQ(errno, ENOENT);
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
// File rename
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, RenameFile) {
  const std::string old_name = "old.txt";
  const std::string new_name = "new.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(old_name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(old_name, content), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(old_name, &before), 0);

  ASSERT_EQ(fixture_.Rename(old_name, new_name), 0);

  EXPECT_EQ(fixture_.OpenFile(old_name, O_RDONLY), -1);
  EXPECT_EQ(errno, ENOENT);

  struct stat after;
  ASSERT_EQ(fixture_.Stat(new_name, &after), 0);
  EXPECT_EQ(after.st_ino, before.st_ino);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_EQ(after.st_nlink, before.st_nlink);
  EXPECT_EQ(after.st_size, before.st_size);
  EXPECT_TRUE(fixture_.FileEquals(new_name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, RenameFileCrossDir) {
  const std::string src = "src";
  const std::string dst = "dst";
  const std::string file_name = "f.txt";
  const std::string src_path = src + "/" + file_name;
  const std::string dst_path = dst + "/" + file_name;
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.MkDir(src, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(dst, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile(src_path, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(src_path, content), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(src_path, &before), 0);

  ASSERT_EQ(fixture_.Rename(src_path, dst_path), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat(src_path, &st), 0);
  EXPECT_EQ(errno, ENOENT);

  struct stat after;
  ASSERT_EQ(fixture_.Stat(dst_path, &after), 0);
  EXPECT_EQ(after.st_ino, before.st_ino);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_EQ(after.st_nlink, before.st_nlink);
  EXPECT_EQ(after.st_size, before.st_size);
  EXPECT_TRUE(fixture_.FileEquals(dst_path, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, RenameOverExistingFile) {
  const std::string old_name = "old.txt";
  const std::string new_name = "new.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  const auto overwritten = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(old_name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(old_name, content), 0);
  ASSERT_EQ(fixture_.CreateFile(new_name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(new_name, overwritten), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(old_name, &before), 0);

  ASSERT_EQ(fixture_.Rename(old_name, new_name), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat(old_name, &st), 0);
  EXPECT_EQ(errno, ENOENT);

  ASSERT_EQ(fixture_.Stat(new_name, &st), 0);
  EXPECT_EQ(st.st_ino, before.st_ino);
  EXPECT_EQ(st.st_mode, before.st_mode);
  EXPECT_EQ(st.st_nlink, before.st_nlink);
  EXPECT_EQ(st.st_size, before.st_size);
  EXPECT_TRUE(fixture_.FileEquals(new_name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, RenameFileToSelf) {
  const std::string name = "f.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(name, &before), 0);

  ASSERT_EQ(fixture_.Rename(name, name), 0);

  struct stat after;
  ASSERT_EQ(fixture_.Stat(name, &after), 0);
  EXPECT_EQ(after.st_ino, before.st_ino);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_EQ(after.st_size, before.st_size);
  EXPECT_TRUE(fixture_.FileEquals(name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, RenameNonexistentSource) {
  ASSERT_EQ(fixture_.Rename("noent", "dst"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, RenameTargetNameTooLong) {
  auto limits = fixture_.GetLimits();
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  const std::string name = "src.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);
  std::string long_name(limits.max_name_length + 1, 'x');
  ASSERT_EQ(fixture_.Rename(name, long_name), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
  EXPECT_TRUE(fixture_.FileEquals(name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, RenameFileOverDir) {
  ASSERT_EQ(fixture_.CreateFile("f.txt", kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.Rename("f.txt", "d"), -1);
  EXPECT_EQ(errno, EISDIR);
}

TEST_F(FileOpsTest, RenameDeeplyNested) {
  const std::string dir = "a";
  const std::string src_path = dir + "/b/c/deep.txt";
  const std::string dst_path = dir + "/flat.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.MkDir(dir, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(dir + "/b", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(dir + "/b/c", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile(src_path, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(src_path, content), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(src_path, &before), 0);

  ASSERT_EQ(fixture_.Rename(src_path, dst_path), 0);

  struct stat after;
  ASSERT_EQ(fixture_.Stat(dst_path, &after), 0);
  EXPECT_EQ(after.st_ino, before.st_ino);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_EQ(after.st_nlink, before.st_nlink);
  EXPECT_EQ(after.st_size, before.st_size);
  EXPECT_TRUE(fixture_.FileEquals(dst_path, content.size(), Fixture::Hash64(content)));
}

// ────────────────────────────────────────────────────────────────
// Symlink
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, SymlinkCreate) {
  const std::string target = "target.txt";
  const std::string link = "link.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(target, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(target, content), 0);
  ASSERT_EQ(fixture_.Symlink(target, link), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Lstat(link, &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));
  EXPECT_EQ(st.st_size, static_cast<off_t>(target.size()));
}

TEST_F(FileOpsTest, SymlinkReadlink) {
  const std::string target = "target.txt";
  const std::string link = "link.txt";
  ASSERT_EQ(fixture_.CreateFile(target, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.Symlink(target, link), 0);

  std::string result;
  ASSERT_EQ(fixture_.Readlink(link, &result), 0);
  EXPECT_EQ(result, target);
}

TEST_F(FileOpsTest, SymlinkSurvivesTargetDeletion) {
  // Symlink should remain intact after the target is deleted:
  // lstat still sees it as a symlink, and readlink returns the original path.
  const std::string target = "target.txt";
  const std::string link = "link.txt";
  ASSERT_EQ(fixture_.CreateFile(target, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.Symlink(target, link), 0);

  // Remove the target file.
  ASSERT_EQ(fixture_.UnlinkFile(target), 0);

  // Symlink should still exist.
  struct stat st;
  ASSERT_EQ(fixture_.Lstat(link, &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));

  // readlink should still return the original target path.
  std::string result;
  ASSERT_EQ(fixture_.Readlink(link, &result), 0);
  EXPECT_EQ(result, target);
}

TEST_F(FileOpsTest, SymlinkNonexistentTarget) {
  // Symlink to a non-existent target is valid.
  ASSERT_EQ(fixture_.Symlink("noent", "dangling"), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Lstat("dangling", &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));
}

TEST_F(FileOpsTest, SymlinkCrossDir) {
  // Symlink target and link are in different directories.
  // The symlink stores a relative path; readlink returns exactly that path.
  const std::string sub_dir = "sub";
  const std::string target_path = sub_dir + "/target.txt";
  const std::string link_name = "link.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);

  ASSERT_EQ(fixture_.MkDir(sub_dir, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile(target_path, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(target_path, content), 0);

  // Symlink at root pointing into sub/.
  ASSERT_EQ(fixture_.Symlink(target_path, link_name), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Lstat(link_name, &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));

  std::string result;
  ASSERT_EQ(fixture_.Readlink(link_name, &result), 0);
  EXPECT_EQ(result, target_path);

  // Content is reachable through the symlink.
  EXPECT_TRUE(fixture_.FileEquals(link_name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, SymlinkToSelf) {
  // A symlink may point to itself; creation succeeds but dereferencing loops.
  const std::string name = "self";
  ASSERT_EQ(fixture_.Symlink(name, name), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Lstat(name, &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));

  std::string result;
  ASSERT_EQ(fixture_.Readlink(name, &result), 0);
  EXPECT_EQ(result, name);
}

// ────────────────────────────────────────────────────────────────
// Hardlink
// ────────────────────────────────────────────────────────────────

TEST_F(FileOpsTest, HardlinkCreate) {
  const std::string old_name = "orig.txt";
  const std::string new_name = "link.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(old_name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(old_name, content), 0);

  struct stat before;
  ASSERT_EQ(fixture_.Stat(old_name, &before), 0);
  nlink_t old_nlink = before.st_nlink;

  ASSERT_EQ(fixture_.HardLink(old_name, new_name), 0);

  // Both names share the same inode.
  struct stat after_orig, after_link;
  ASSERT_EQ(fixture_.Stat(old_name, &after_orig), 0);
  ASSERT_EQ(fixture_.Stat(new_name, &after_link), 0);
  EXPECT_EQ(after_orig.st_ino, after_link.st_ino);
  EXPECT_EQ(after_orig.st_nlink, old_nlink + 1);
  EXPECT_EQ(after_link.st_nlink, after_orig.st_nlink);

  // Content is accessible through both names.
  EXPECT_TRUE(fixture_.FileEquals(old_name, content.size(), Fixture::Hash64(content)));
  EXPECT_TRUE(fixture_.FileEquals(new_name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, HardlinkSurvivesOriginalUnlink) {
  // Unlinking the original name should decrement nlink but leave the
  // hardlink intact — same inode, same content.
  const std::string orig = "orig.txt";
  const std::string link = "link.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(orig, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(orig, content), 0);
  ASSERT_EQ(fixture_.HardLink(orig, link), 0);

  struct stat link_before;
  ASSERT_EQ(fixture_.Stat(link, &link_before), 0);
  const ino_t link_ino = link_before.st_ino;
  const nlink_t link_nlink = link_before.st_nlink;

  // Unlink the original name.
  ASSERT_EQ(fixture_.UnlinkFile(orig), 0);

  // Original name is gone.
  struct stat st;
  EXPECT_NE(fixture_.Stat(orig, &st), 0);
  EXPECT_EQ(errno, ENOENT);

  // Hardlink still exists, same inode, nlink decreased by 1.
  ASSERT_EQ(fixture_.Stat(link, &st), 0);
  EXPECT_EQ(st.st_ino, link_ino);
  EXPECT_EQ(st.st_nlink, link_nlink - 1);
  EXPECT_TRUE(fixture_.FileEquals(link, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, HardlinkSurvivesLinkUnlink) {
  // Symmetric: unlinking the hardlink name leaves the original intact.
  const std::string orig = "orig.txt";
  const std::string link = "link.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(orig, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(orig, content), 0);
  ASSERT_EQ(fixture_.HardLink(orig, link), 0);

  struct stat orig_before;
  ASSERT_EQ(fixture_.Stat(orig, &orig_before), 0);
  const ino_t orig_ino = orig_before.st_ino;
  const nlink_t orig_nlink = orig_before.st_nlink;

  // Unlink the hardlink name.
  ASSERT_EQ(fixture_.UnlinkFile(link), 0);

  // Hardlink name is gone.
  struct stat st;
  EXPECT_NE(fixture_.Stat(link, &st), 0);
  EXPECT_EQ(errno, ENOENT);

  // Original still exists, same inode, nlink decreased by 1.
  ASSERT_EQ(fixture_.Stat(orig, &st), 0);
  EXPECT_EQ(st.st_ino, orig_ino);
  EXPECT_EQ(st.st_nlink, orig_nlink - 1);
  EXPECT_TRUE(fixture_.FileEquals(orig, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, HardlinkNonexistentSource) {
  ASSERT_EQ(fixture_.HardLink("noent", "link.txt"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileOpsTest, HardlinkToSelf) {
  // Hard-linking a file to itself should fail with EEXIST.
  const std::string name = "f.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, content), 0);

  ASSERT_EQ(fixture_.HardLink(name, name), -1);
  EXPECT_EQ(errno, EEXIST);

  // File is unchanged.
  EXPECT_TRUE(fixture_.FileEquals(name, content.size(), Fixture::Hash64(content)));
}

TEST_F(FileOpsTest, HardlinkCrossDir) {
  // Hardlinks can span directories; both names share the same inode.
  const std::string dir_a = "a";
  const std::string dir_b = "b";
  const std::string src_path = dir_a + "/f.txt";
  const std::string dst_path = dir_b + "/f.txt";
  const auto content = Fixture::GenerateRandomData(kSmallContentLen, Fixture::RandomMode::kUpTo);

  ASSERT_EQ(fixture_.MkDir(dir_a, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(dir_b, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile(src_path, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(src_path, content), 0);

  ASSERT_EQ(fixture_.HardLink(src_path, dst_path), 0);

  struct stat st_a, st_b;
  ASSERT_EQ(fixture_.Stat(src_path, &st_a), 0);
  ASSERT_EQ(fixture_.Stat(dst_path, &st_b), 0);
  EXPECT_EQ(st_a.st_ino, st_b.st_ino);
  EXPECT_EQ(st_a.st_nlink, static_cast<nlink_t>(2));
  EXPECT_EQ(st_b.st_nlink, static_cast<nlink_t>(2));

  // Unlink one; the other survives with nlink=1.
  ASSERT_EQ(fixture_.UnlinkFile(src_path), 0);
  ASSERT_EQ(fixture_.Stat(dst_path, &st_b), 0);
  EXPECT_EQ(st_b.st_nlink, static_cast<nlink_t>(1));
  EXPECT_TRUE(fixture_.FileEquals(dst_path, content.size(), Fixture::Hash64(content)));
}
