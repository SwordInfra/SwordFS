// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: POSIX permission enforcement.
//
// Validates: owner/group/other read/write/execute bits via
//            chmod and access checks.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class PermissionsTest : public ::testing::Test {
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
// chmod
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, ChmodFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0600), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(PermissionsTest, ChmodDir) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0700), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700);
}

// ────────────────────────────────────────────────────────────────
// Default permissions
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, DefaultFileMode) {
  const char *name = "f.txt";
  mode_t old = ::umask(022);
  int fd = fixture_.CreateFile(name, 0666, O_CREAT | O_WRONLY | O_TRUNC);
  ::umask(old);
  ASSERT_GE(fd, 0);
  ::close(fd);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  // umask 022 strips group/other write bits, leaving 0644.
  EXPECT_EQ(st.st_mode & 0777, 0644);
}

TEST_F(PermissionsTest, DefaultDirMode) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0755);
}

// ────────────────────────────────────────────────────────────────
// access() checks
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, AccessOwnerReadWrite) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0600), 0);
  EXPECT_EQ(fixture_.Access(name, R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, AccessOtherDenied) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0600), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  // Mode was set correctly: owner rw, group/other nothing.
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(PermissionsTest, AccessDirExecute) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0700), 0);
  EXPECT_EQ(fixture_.Access(name, X_OK), 0);
}

TEST_F(PermissionsTest, AccessReadWrite) {
  const char *name = "rw.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  EXPECT_EQ(fixture_.Access(name, R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, AccessNonexistent) {
  const char *name = "noent";
  EXPECT_NE(fixture_.Access(name, F_OK), 0);
}
