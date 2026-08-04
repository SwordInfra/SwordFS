// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: basic CRUD filesystem operations.
//
// Validates: mkdir, rmdir, creat/write/read/unlink, stat, statfs,
//            truncate, readdir.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

// ────────────────────────────────────────────────────────────────
// Test fixture — one fresh SwordFS mount per test case
// ────────────────────────────────────────────────────────────────

class BasicOpsTest : public ::testing::Test {
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
// Directory operations
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, MkdirAndRmdir) {
  ASSERT_TRUE(fixture_.Mkdir("subdir"));
  auto st = fixture_.Stat("subdir");
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  ASSERT_TRUE(fixture_.Rmdir("subdir"));
  st = fixture_.Stat("subdir");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));  // ENOENT → zeroed stat
}

TEST_F(BasicOpsTest, MkdirNested) {
  ASSERT_TRUE(fixture_.Mkdir("a"));
  ASSERT_TRUE(fixture_.Mkdir("a/b"));
  ASSERT_TRUE(fixture_.Mkdir("a/b/c"));

  auto st = fixture_.Stat("a/b/c");
  EXPECT_TRUE(S_ISDIR(st.st_mode));
}

// ────────────────────────────────────────────────────────────────
// File operations
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, CreateWriteReadUnlink) {
  ASSERT_TRUE(fixture_.WriteFile("hello.txt", "Hello, SwordFS!"));
  EXPECT_EQ(fixture_.ReadFile("hello.txt"), "Hello, SwordFS!");

  ASSERT_TRUE(fixture_.Unlink("hello.txt"));
  auto st = fixture_.Stat("hello.txt");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));
}

TEST_F(BasicOpsTest, WriteEmptyFile) {
  ASSERT_TRUE(fixture_.WriteFile("empty.txt", ""));
  EXPECT_EQ(fixture_.ReadFile("empty.txt"), "");

  auto st = fixture_.Stat("empty.txt");
  EXPECT_EQ(st.st_size, 0);
}

TEST_F(BasicOpsTest, WriteLargeData) {
  // Write 1 MiB of data — fits in a single chunk but exercises the write path.
  std::string data(1024 * 1024, 'X');
  ASSERT_TRUE(fixture_.WriteFile("large.bin", data));
  EXPECT_EQ(fixture_.ReadFile("large.bin"), data);
}

TEST_F(BasicOpsTest, OverwriteFile) {
  ASSERT_TRUE(fixture_.WriteFile("f.txt", "first"));
  ASSERT_TRUE(fixture_.WriteFile("f.txt", "second"));
  EXPECT_EQ(fixture_.ReadFile("f.txt"), "second");
}

TEST_F(BasicOpsTest, AppendToFile) {
  // Write initial content.
  ASSERT_TRUE(fixture_.WriteFile("append.txt", "hello"));

  // Use O_APPEND via POSIX.
  std::string path = fixture_.MountPath("append.txt");
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  const char* extra = " world";
  ssize_t n = ::write(fd, extra, 6);
  ::close(fd);
  ASSERT_EQ(n, 6);

  EXPECT_EQ(fixture_.ReadFile("append.txt"), "hello world");
}

// ────────────────────────────────────────────────────────────────
// stat / statvfs
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, StatRoot) {
  auto st = fixture_.Stat("");
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_GT(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(st.st_nlink, static_cast<nlink_t>(2));  // . and ..
}

TEST_F(BasicOpsTest, StatFile) {
  ASSERT_TRUE(fixture_.WriteFile("s.txt", "data"));
  auto st = fixture_.Stat("s.txt");
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_size, 4);
}

TEST_F(BasicOpsTest, StatfsReturnsValidData) {
  auto sv = fixture_.Statfs();
  EXPECT_GT(sv.f_bsize, static_cast<unsigned long>(0));
  EXPECT_GT(sv.f_blocks, static_cast<fsblkcnt_t>(0));
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, TruncateShrink) {
  ASSERT_TRUE(fixture_.WriteFile("t.txt", "hello world"));
  ASSERT_TRUE(fixture_.Truncate("t.txt", 5));
  EXPECT_EQ(fixture_.ReadFile("t.txt"), "hello");
}

TEST_F(BasicOpsTest, TruncateExtend) {
  ASSERT_TRUE(fixture_.WriteFile("t.txt", "hi"));
  ASSERT_TRUE(fixture_.Truncate("t.txt", 10));
  auto st = fixture_.Stat("t.txt");
  EXPECT_EQ(st.st_size, 10);
}

// ────────────────────────────────────────────────────────────────
// readdir
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, ReaddirListsEntries) {
  ASSERT_TRUE(fixture_.Mkdir("d1"));
  ASSERT_TRUE(fixture_.Mkdir("d2"));
  ASSERT_TRUE(fixture_.WriteFile("f1.txt", ""));
  ASSERT_TRUE(fixture_.WriteFile("f2.txt", ""));

  auto entries = fixture_.ReadDir();
  EXPECT_EQ(entries.size(), 4u);
}

TEST_F(BasicOpsTest, ReaddirEmpty) {
  auto entries = fixture_.ReadDir();
  EXPECT_TRUE(entries.empty());
}

// ────────────────────────────────────────────────────────────────
// access
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, AccessReadWrite) {
  ASSERT_TRUE(fixture_.WriteFile("rw.txt", "data"));
  EXPECT_EQ(fixture_.Access("rw.txt", R_OK | W_OK), 0);
}

TEST_F(BasicOpsTest, AccessNoEnt) {
  EXPECT_NE(fixture_.Access("nonexistent", F_OK), 0);
}

// ────────────────────────────────────────────────────────────────
// Error cases
// ────────────────────────────────────────────────────────────────

TEST_F(BasicOpsTest, RmdirNonEmpty) {
  ASSERT_TRUE(fixture_.Mkdir("d"));
  ASSERT_TRUE(fixture_.WriteFile("d/f.txt", "x"));
  EXPECT_FALSE(fixture_.Rmdir("d"));  // ENOTEMPTY
}

TEST_F(BasicOpsTest, UnlinkNonexistent) {
  EXPECT_FALSE(fixture_.Unlink("no_such_file"));
}

TEST_F(BasicOpsTest, MkdirExisting) {
  ASSERT_TRUE(fixture_.Mkdir("d"));
  EXPECT_FALSE(fixture_.Mkdir("d"));  // EEXIST
}

TEST_F(BasicOpsTest, FileInRootIsFile) {
  ASSERT_TRUE(fixture_.WriteFile("plain.txt", "text"));
  auto st = fixture_.Stat("plain.txt");
  EXPECT_FALSE(S_ISDIR(st.st_mode));
  EXPECT_TRUE(S_ISREG(st.st_mode));
}
