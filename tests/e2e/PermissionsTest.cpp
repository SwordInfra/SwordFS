// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: POSIX permission enforcement.
//
// Validates: owner/group/other read/write/execute bits via
//            chmod and access checks.

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
  ASSERT_TRUE(fixture_.WriteFile("f.txt", "data"));
  ASSERT_TRUE(fixture_.Chmod("f.txt", 0600));

  auto st = fixture_.Stat("f.txt");
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(PermissionsTest, ChmodDir) {
  ASSERT_TRUE(fixture_.Mkdir("d", 0755));
  ASSERT_TRUE(fixture_.Chmod("d", 0700));

  auto st = fixture_.Stat("d");
  EXPECT_EQ(st.st_mode & 0777, 0700);
}

// ────────────────────────────────────────────────────────────────
// Default permissions
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, DefaultFileMode) {
  ASSERT_TRUE(fixture_.WriteFile("f.txt", "data"));
  auto st = fixture_.Stat("f.txt");
  // Files are created with 0644 by default (minus umask).
  EXPECT_EQ(st.st_mode & 0777, 0644);
}

TEST_F(PermissionsTest, DefaultDirMode) {
  ASSERT_TRUE(fixture_.Mkdir("d"));
  auto st = fixture_.Stat("d");
  EXPECT_EQ(st.st_mode & 0777, 0755);
}

// ────────────────────────────────────────────────────────────────
// access() checks
// ────────────────────────────────────────────────────────────────

TEST_F(PermissionsTest, AccessOwnerReadWrite) {
  ASSERT_TRUE(fixture_.WriteFile("f.txt", "data"));
  ASSERT_TRUE(fixture_.Chmod("f.txt", 0600));
  EXPECT_EQ(fixture_.Access("f.txt", R_OK | W_OK), 0);
}

TEST_F(PermissionsTest, AccessOtherDenied) {
  ASSERT_TRUE(fixture_.WriteFile("f.txt", "data"));
  ASSERT_TRUE(fixture_.Chmod("f.txt", 0600));
  auto st = fixture_.Stat("f.txt");
  // Mode was set correctly: owner rw, group/other nothing.
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(PermissionsTest, AccessDirExecute) {
  ASSERT_TRUE(fixture_.Mkdir("d", 0700));
  EXPECT_EQ(fixture_.Access("d", X_OK), 0);
}

TEST_F(PermissionsTest, AccessNonexistent) {
  EXPECT_NE(fixture_.Access("noent", F_OK), 0);
}
