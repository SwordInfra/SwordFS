// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: POSIX permission enforcement.
//
// Validates: owner/group/other read/write/execute bits via
//            chmod and access checks.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>

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
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0644);

  ASSERT_EQ(fixture_.Chmod(name, 0600), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
  EXPECT_EQ(fixture_.Access(name, R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, ChmodDir) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, 0755), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0700), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700);
  EXPECT_EQ(fixture_.Access(name, X_OK), 0);
}

TEST_F(PermissionsTest, ChmodOwnerBits) {
  const char *name = "owner.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);

  // 0444: owner r--.
  ASSERT_EQ(fixture_.Chmod(name, 0444), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0444);
  EXPECT_EQ(fixture_.Access(name, R_OK), 0);
  EXPECT_NE(fixture_.Access(name, W_OK), 0);

  // 0222: owner -w-.
  ASSERT_EQ(fixture_.Chmod(name, 0222), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0222);
  EXPECT_EQ(fixture_.Access(name, W_OK), 0);
  EXPECT_NE(fixture_.Access(name, R_OK), 0);

  // 0111: owner --x.
  ASSERT_EQ(fixture_.Chmod(name, 0111), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0111);
  EXPECT_EQ(fixture_.Access(name, X_OK), 0);
  EXPECT_NE(fixture_.Access(name, R_OK), 0);
}

TEST_F(PermissionsTest, ChmodNoPerms) {
  const char *name = "noperm.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0000), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0000);
  EXPECT_NE(fixture_.Access(name, R_OK), 0);
  EXPECT_NE(fixture_.Access(name, W_OK), 0);
}

TEST_F(PermissionsTest, ChmodAllPerms) {
  const char *name = "all.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0777), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0777);
  EXPECT_EQ(fixture_.Access(name, R_OK | W_OK | X_OK), 0);
}

TEST_F(PermissionsTest, ChmodGroupBits) {
  // access() always checks owner bits for the file owner, so we can
  // only verify that group r/w/x are stored correctly via stat.
  const char *name = "grp.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);

  // 0640: owner rw-, group r--.
  ASSERT_EQ(fixture_.Chmod(name, 0640), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0640);

  // 0620: owner rw-, group -w-.
  ASSERT_EQ(fixture_.Chmod(name, 0620), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0620);

  // 0610: owner rw-, group --x.
  ASSERT_EQ(fixture_.Chmod(name, 0610), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0610);
}

TEST_F(PermissionsTest, ChmodOtherBits) {
  // access() checks owner bits for the file owner, so when owner is
  // --- the process loses all access regardless of other bits.
  const char *name = "oth.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);

  // 0004: owner ---, other r--.
  ASSERT_EQ(fixture_.Chmod(name, 0004), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0004);
  EXPECT_NE(fixture_.Access(name, R_OK), 0);

  // 0002: owner ---, other -w-.
  ASSERT_EQ(fixture_.Chmod(name, 0002), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0002);
  EXPECT_NE(fixture_.Access(name, W_OK), 0);

  // 0001: owner ---, other --x.
  ASSERT_EQ(fixture_.Chmod(name, 0001), 0);
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0001);
  EXPECT_NE(fixture_.Access(name, X_OK), 0);
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
  mode_t old = ::umask(022);
  ASSERT_EQ(fixture_.MkDir(name, 0777), 0);
  ::umask(old);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  // umask 022 strips group/other write bits, leaving 0755.
  EXPECT_EQ(st.st_mode & 0777, 0755);
}

// ────────────────────────────────────────────────────────────────
// access() — no chmod
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, AccessReadWrite) {
  const char *name = "rw.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  EXPECT_EQ(fixture_.Access(name, R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, AccessNonexistent) {
  const char *name = "noent";
  EXPECT_NE(fixture_.Access(name, F_OK), 0);
}
