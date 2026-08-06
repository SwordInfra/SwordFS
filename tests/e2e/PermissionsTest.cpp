// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: POSIX permission enforcement.
//
// Validates: owner/group/other read/write/execute bits via
//            chmod and access checks.

#include <gtest/gtest.h>

#include <fcntl.h>
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
  ASSERT_EQ(fixture_.WriteFile("f.txt", "data"), 0);
  ASSERT_EQ(::chmod(fixture_.MountPath("f.txt").c_str(), 0600), 0);

  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("f.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(PermissionsTest, ChmodDir) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0755), 0);
  ASSERT_EQ(::chmod(fixture_.MountPath("d").c_str(), 0700), 0);

  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("d").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700);
}

// ────────────────────────────────────────────────────────────────
// Default permissions
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, DefaultFileMode) {
  mode_t old = ::umask(022);
  int fd = ::creat(fixture_.MountPath("f.txt").c_str(), 0666);
  ::umask(old);
  ASSERT_GE(fd, 0);
  ::close(fd);

  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("f.txt").c_str(), &st), 0);
  // umask 022 strips group/other write bits, leaving 0644.
  EXPECT_EQ(st.st_mode & 0777, 0644);
}

TEST_F(PermissionsTest, DefaultDirMode) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0755), 0);
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("d").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0755);
}

// ────────────────────────────────────────────────────────────────
// access() checks
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, AccessOwnerReadWrite) {
  ASSERT_EQ(fixture_.WriteFile("f.txt", "data"), 0);
  ASSERT_EQ(::chmod(fixture_.MountPath("f.txt").c_str(), 0600), 0);
  EXPECT_EQ(::access(fixture_.MountPath("f.txt").c_str(), R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, AccessOtherDenied) {
  ASSERT_EQ(fixture_.WriteFile("f.txt", "data"), 0);
  ASSERT_EQ(::chmod(fixture_.MountPath("f.txt").c_str(), 0600), 0);
  struct stat st;
  ASSERT_EQ(::stat(fixture_.MountPath("f.txt").c_str(), &st), 0);
  // Mode was set correctly: owner rw, group/other nothing.
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(PermissionsTest, AccessDirExecute) {
  ASSERT_EQ(::mkdir(fixture_.MountPath("d").c_str(), 0700), 0);
  EXPECT_EQ(::access(fixture_.MountPath("d").c_str(), X_OK), 0);
}

TEST_F(PermissionsTest, AccessReadWrite) {
  ASSERT_EQ(fixture_.WriteFile("rw.txt", "data"), 0);
  EXPECT_EQ(::access(fixture_.MountPath("rw.txt").c_str(), R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, AccessNonexistent) {
  EXPECT_NE(::access(fixture_.MountPath("noent").c_str(), F_OK), 0);
}
