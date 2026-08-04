// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: rename operations.
//
// Validates: file rename (same dir), file rename (cross dir),
//            directory rename (same parent), directory rename (cross parent),
//            rename over existing file, rename error cases.

#include <gtest/gtest.h>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class RenameTest : public ::testing::Test {
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
// File rename — same directory
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameFileSameDir) {
  ASSERT_TRUE(fixture_.WriteFile("old.txt", "hello"));
  ASSERT_TRUE(fixture_.Rename("old.txt", "new.txt"));

  auto st = fixture_.Stat("old.txt");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));  // gone

  EXPECT_EQ(fixture_.ReadFile("new.txt"), "hello");
}

// ────────────────────────────────────────────────────────────────
// File rename — cross directory
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameFileCrossDir) {
  ASSERT_TRUE(fixture_.Mkdir("src"));
  ASSERT_TRUE(fixture_.Mkdir("dst"));
  ASSERT_TRUE(fixture_.WriteFile("src/f.txt", "data"));

  ASSERT_TRUE(fixture_.Rename("src/f.txt", "dst/f.txt"));

  auto st = fixture_.Stat("src/f.txt");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(fixture_.ReadFile("dst/f.txt"), "data");
}

// ────────────────────────────────────────────────────────────────
// Directory rename — same parent
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameDirSameParent) {
  ASSERT_TRUE(fixture_.Mkdir("olddir"));
  ASSERT_TRUE(fixture_.WriteFile("olddir/f.txt", "x"));

  ASSERT_TRUE(fixture_.Rename("olddir", "newdir"));

  auto st = fixture_.Stat("olddir");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(fixture_.ReadFile("newdir/f.txt"), "x");
}

// ────────────────────────────────────────────────────────────────
// Directory rename — cross parent
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameDirCrossParent) {
  ASSERT_TRUE(fixture_.Mkdir("a"));
  ASSERT_TRUE(fixture_.Mkdir("a/b"));
  ASSERT_TRUE(fixture_.WriteFile("a/b/f.txt", "data"));

  ASSERT_TRUE(fixture_.Rename("a/b", "b"));

  auto st = fixture_.Stat("a/b");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(fixture_.ReadFile("b/f.txt"), "data");
}

// ────────────────────────────────────────────────────────────────
// Rename over existing (replace)
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameOverExistingFile) {
  ASSERT_TRUE(fixture_.WriteFile("old.txt", "old"));
  ASSERT_TRUE(fixture_.WriteFile("new.txt", "new"));

  ASSERT_TRUE(fixture_.Rename("old.txt", "new.txt"));

  EXPECT_EQ(fixture_.ReadFile("new.txt"), "old");
  auto st = fixture_.Stat("old.txt");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));
}

TEST_F(RenameTest, RenameDirOverEmptyDir) {
  ASSERT_TRUE(fixture_.Mkdir("a"));
  ASSERT_TRUE(fixture_.WriteFile("a/f.txt", "a"));
  ASSERT_TRUE(fixture_.Mkdir("b"));

  ASSERT_TRUE(fixture_.Rename("a", "b"));

  auto st = fixture_.Stat("a");
  EXPECT_EQ(st.st_ino, static_cast<ino_t>(0));
  EXPECT_EQ(fixture_.ReadFile("b/f.txt"), "a");
}

// ────────────────────────────────────────────────────────────────
// Error cases
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameNonexistentSource) {
  EXPECT_FALSE(fixture_.Rename("noent", "dst"));
}

TEST_F(RenameTest, RenameDirOverNonEmptyDir) {
  ASSERT_TRUE(fixture_.Mkdir("a"));
  ASSERT_TRUE(fixture_.Mkdir("b"));
  ASSERT_TRUE(fixture_.WriteFile("b/f.txt", "x"));

  EXPECT_FALSE(fixture_.Rename("a", "b"));  // ENOTEMPTY
}

// ────────────────────────────────────────────────────────────────
// Rename within nested directory tree
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameDeeplyNested) {
  ASSERT_TRUE(fixture_.Mkdir("a"));
  ASSERT_TRUE(fixture_.Mkdir("a/b"));
  ASSERT_TRUE(fixture_.Mkdir("a/b/c"));
  ASSERT_TRUE(fixture_.WriteFile("a/b/c/deep.txt", "deep data"));

  ASSERT_TRUE(fixture_.Rename("a/b/c/deep.txt", "a/flat.txt"));
  EXPECT_EQ(fixture_.ReadFile("a/flat.txt"), "deep data");
}
