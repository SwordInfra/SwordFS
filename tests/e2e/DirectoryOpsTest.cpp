// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: directory operations.
//
// Validates: mkdir, rmdir, readdir, truncate-on-dir,
//            directory name length limits.

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <dirent.h>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class DirectoryOpsTest : public ::testing::Test {
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
// mkdir / rmdir — happy path
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirAndRmdir) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("subdir").c_str(), 0755), 0);
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("subdir").c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  ASSERT_EQ(::rmdir(fixture_.MountPath("subdir").c_str()), 0);
  EXPECT_NE(::stat(fixture_.MountPath("subdir").c_str(), &st), 0);
}

TEST_F(DirectoryOpsTest, MkdirNested) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("a").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(fixture_.MountPath("a/b").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(fixture_.MountPath("a/b/c").c_str(), 0755), 0);

  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("a/b/c").c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
}

// ────────────────────────────────────────────────────────────────
// mkdir — error cases
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirExisting) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0755), -1);
  EXPECT_EQ(errno, EEXIST);
}

TEST_F(DirectoryOpsTest, MkdirUnderFile) {
  ASSERT_EQ(fixture_.WriteFile("f.txt", "data"), 0);
  ASSERT_EQ(::mkdir(fixture_.MountPath("f.txt/sub").c_str(), 0755), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, MkdirUnderNonexistent) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("noent/sub").c_str(), 0755), -1);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// rmdir — error cases
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RmdirNonEmpty) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0755), 0);
  ASSERT_EQ(fixture_.WriteFile("d/f.txt", "x"), 0);
  ASSERT_EQ(::rmdir(fixture_.MountPath("d").c_str()), -1);
  EXPECT_EQ(errno, ENOTEMPTY);
}

TEST_F(DirectoryOpsTest, RmdirNonexistent) {
  ASSERT_EQ(::rmdir(fixture_.MountPath("no_such_dir").c_str()), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(DirectoryOpsTest, RmdirOnFile) {
  ASSERT_EQ(fixture_.WriteFile("f.txt", "data"), 0);
  ASSERT_EQ(::rmdir(fixture_.MountPath("f.txt").c_str()), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, RmdirRoot) {
  ASSERT_EQ(::rmdir(fixture_.MountPath(".").c_str()), -1);
  EXPECT_TRUE(errno == EBUSY || errno == EINVAL);
}

// ────────────────────────────────────────────────────────────────
// readdir
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, ReaddirListsEntries) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d1").c_str(), 0755), 0);
  ASSERT_EQ(::mkdir(fixture_.MountPath("d2").c_str(), 0755), 0);
  ASSERT_EQ(fixture_.WriteFile("f1.txt", ""), 0);
  ASSERT_EQ(fixture_.WriteFile("f2.txt", ""), 0);

  std::vector<std::string> entries; fixture_.ReadDir(".", &entries);
  EXPECT_EQ(entries.size(), 4u);
}

TEST_F(DirectoryOpsTest, ReaddirEmpty) {
  std::vector<std::string> entries; fixture_.ReadDir(".", &entries);
  EXPECT_TRUE(entries.empty());
}

TEST_F(DirectoryOpsTest, ReaddirOnFile) {
  ASSERT_EQ(fixture_.WriteFile("f.txt", "data"), 0);
  std::string path = fixture_.MountPath("f.txt");
  DIR* dp = ::opendir(path.c_str());
  ASSERT_EQ(dp, nullptr);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, ReaddirNonExistent) {
  std::string path = fixture_.MountPath("noent");
  DIR* dp = ::opendir(path.c_str());
  ASSERT_EQ(dp, nullptr);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// truncate on directory
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, TruncateDir) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0755), 0);
  ASSERT_EQ(::truncate(fixture_.MountPath("d").c_str(), 0), -1);
  EXPECT_EQ(errno, EISDIR);
}

// ────────────────────────────────────────────────────────────────
// Name length limits
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirNameAtLimit) {
  std::string name(255, 'x');
  ASSERT_EQ(::mkdir(fixture_.MountPath(name).c_str(), 0755), 0);
  ASSERT_EQ(::rmdir(fixture_.MountPath(name).c_str()), 0);
}

TEST_F(DirectoryOpsTest, MkdirNameTooLong) {
  std::string name(256, 'x');
  ASSERT_EQ(::mkdir(fixture_.MountPath(name).c_str(), 0755), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
}
