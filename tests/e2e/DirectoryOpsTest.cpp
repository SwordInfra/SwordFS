// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: directory operations.
//
// Validates: mkdir, rmdir, readdir, truncate-on-dir,
//            directory name length limits.

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

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
  const char *name = "subdir";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  ASSERT_EQ(fixture_.RmDir(name), 0);
  EXPECT_NE(fixture_.Stat(name, &st), 0);
}

TEST_F(DirectoryOpsTest, MkdirNested) {
  ASSERT_EQ(fixture_.MkDir("a", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("a/b", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("a/b/c", 0755), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat("a/b/c", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
}

// ────────────────────────────────────────────────────────────────
// mkdir — error cases
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirExisting) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.MkDir(name, 0755), -1);
  EXPECT_EQ(errno, EEXIST);
}

TEST_F(DirectoryOpsTest, MkdirUnderFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.MkDir(name + std::string("/sub"), 0755), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, MkdirUnderNonexistent) {
  ASSERT_EQ(fixture_.MkDir("noent/sub", 0755), -1);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// rmdir — error cases
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RmdirNonEmpty) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.CreateFile(name + std::string("/f.txt"), 0644, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.RmDir(name), -1);
  EXPECT_EQ(errno, ENOTEMPTY);
}

TEST_F(DirectoryOpsTest, RmdirNonexistent) {
  ASSERT_EQ(fixture_.RmDir("no_such_dir"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(DirectoryOpsTest, RmdirOnFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.RmDir(name), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, RmdirRoot) {
  ASSERT_EQ(fixture_.RmDir("."), -1);
  EXPECT_TRUE(errno == EBUSY || errno == EINVAL);
}

// ────────────────────────────────────────────────────────────────
// readdir
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, ReaddirListsEntries) {
  ASSERT_EQ(fixture_.MkDir("d1", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("d2", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("f1.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("f1.txt", ""), 0);
  ASSERT_EQ(fixture_.CreateFile("f2.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("f2.txt", ""), 0);

  std::vector<std::string> entries;
  fixture_.ReadDir(".", &entries);
  EXPECT_EQ(entries.size(), 4u);
}

TEST_F(DirectoryOpsTest, ReaddirEmpty) {
  std::vector<std::string> entries;
  fixture_.ReadDir(".", &entries);
  EXPECT_TRUE(entries.empty());
}

TEST_F(DirectoryOpsTest, ReaddirOnFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  std::vector<std::string> entries;
  ASSERT_EQ(fixture_.ReadDir(name, &entries), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, ReaddirNonExistent) {
  std::vector<std::string> entries;
  ASSERT_EQ(fixture_.ReadDir("noent", &entries), -1);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// truncate on directory
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, TruncateDir) {
  ASSERT_EQ(fixture_.MkDir("d", 0755), 0);
  ASSERT_EQ(fixture_.Truncate("d", 0), -1);
  EXPECT_EQ(errno, EISDIR);
}

// ────────────────────────────────────────────────────────────────
// Name length limits
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirNameAtLimit) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length, 'x');
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.RmDir(name), 0);
}

TEST_F(DirectoryOpsTest, MkdirNameTooLong) {
  std::string name(256, 'x');
  ASSERT_EQ(fixture_.MkDir(name, 0755), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
}
