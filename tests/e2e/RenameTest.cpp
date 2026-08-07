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
  ASSERT_EQ(fixture_.CreateFile("old.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("old.txt", "hello"), 0);
  ASSERT_EQ(fixture_.Rename("old.txt", "new.txt"), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat("old.txt", &st), 0);

  EXPECT_TRUE(fixture_.FileEquals("new.txt", 5, Fixture::Hash64("hello")));
}

// ────────────────────────────────────────────────────────────────
// File rename — cross directory
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameFileCrossDir) {
  ASSERT_EQ(fixture_.MkDir("src", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("dst", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("src/f.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("src/f.txt", "data"), 0);

  ASSERT_EQ(fixture_.Rename("src/f.txt", "dst/f.txt"), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat("src/f.txt", &st), 0);
  EXPECT_TRUE(fixture_.FileEquals("dst/f.txt", 4, Fixture::Hash64("data")));
}

// ────────────────────────────────────────────────────────────────
// Directory rename — same parent
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameDirSameParent) {
  ASSERT_EQ(fixture_.MkDir("olddir", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("olddir/f.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("olddir/f.txt", "x"), 0);

  ASSERT_EQ(fixture_.Rename("olddir", "newdir"), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat("olddir", &st), 0);
  EXPECT_TRUE(fixture_.FileEquals("newdir/f.txt", 1, Fixture::Hash64("x")));
}

// ────────────────────────────────────────────────────────────────
// Directory rename — cross parent
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameDirCrossParent) {
  ASSERT_EQ(fixture_.MkDir("a", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("a/b", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("a/b/f.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("a/b/f.txt", "data"), 0);

  ASSERT_EQ(fixture_.Rename("a/b", "b"), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat("a/b", &st), 0);
  EXPECT_TRUE(fixture_.FileEquals("b/f.txt", 4, Fixture::Hash64("data")));
}

// ────────────────────────────────────────────────────────────────
// Rename over existing (replace)
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameOverExistingFile) {
  ASSERT_EQ(fixture_.CreateFile("old.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("old.txt", "old"), 0);
  ASSERT_EQ(fixture_.CreateFile("new.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("new.txt", "new"), 0);

  ASSERT_EQ(fixture_.Rename("old.txt", "new.txt"), 0);

  EXPECT_TRUE(fixture_.FileEquals("new.txt", 3, Fixture::Hash64("old")));
  struct stat st;
  EXPECT_NE(fixture_.Stat("old.txt", &st), 0);
}

TEST_F(RenameTest, RenameDirOverEmptyDir) {
  ASSERT_EQ(fixture_.MkDir("a", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("a/f.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("a/f.txt", "a"), 0);
  ASSERT_EQ(fixture_.MkDir("b", 0755), 0);

  ASSERT_EQ(fixture_.Rename("a", "b"), 0);

  struct stat st;
  EXPECT_NE(fixture_.Stat("a", &st), 0);
  EXPECT_TRUE(fixture_.FileEquals("b/f.txt", 1, Fixture::Hash64("a")));
}

// ────────────────────────────────────────────────────────────────
// Error cases
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameNonexistentSource) {
  ASSERT_EQ(fixture_.Rename("noent", "dst"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(RenameTest, RenameDirOverNonEmptyDir) {
  ASSERT_EQ(fixture_.MkDir("a", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("b", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("b/f.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("b/f.txt", "x"), 0);

  ASSERT_EQ(fixture_.Rename("a", "b"), -1);
  EXPECT_EQ(errno, ENOTEMPTY);
}

// ────────────────────────────────────────────────────────────────
// Rename within nested directory tree
// ────────────────────────────────────────────────────────────────

TEST_F(RenameTest, RenameDeeplyNested) {
  ASSERT_EQ(fixture_.MkDir("a", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("a/b", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("a/b/c", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("a/b/c/deep.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("a/b/c/deep.txt", "deep data"), 0);

  ASSERT_EQ(fixture_.Rename("a/b/c/deep.txt", "a/flat.txt"), 0);
  EXPECT_TRUE(fixture_.FileEquals("a/flat.txt", 9, Fixture::Hash64("deep data")));
}
