// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for ReadDir / directory listing at the MemMetaImpl level.
// Validates the data layer that feeds into VfsImpl::Readdir/Readdirplus
// (the FUSE formatting fix is in PR #23).

#define FUSE_USE_VERSION 312
#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <fuse_lowlevel.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <set>

#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::SwordFsEntry;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = FUSE_ROOT_ID;

class MemMetaImplReadDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new MemMetaImpl();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override { delete impl_; }

  MemMetaImpl* impl_;
};

// ────────────────────────────────────────────────────────────────
// ReadDir: empty directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirEmpty) {
  std::vector<SwordFsEntry> entries;
  Status st = impl_->ReadDir(kRoot, &entries);
  EXPECT_TRUE(st.ok()) << st.message();
  // Root starts empty.  Note: "." and ".." are added at the VfsImpl
  // level, not in ReadDir — so we expect 0 entries here.
  EXPECT_EQ(entries.size(), 0);
}

// ────────────────────────────────────────────────────────────────
// ReadDir: non-empty directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirWithEntries) {
  constexpr int kFiles = 10;
  for (int i = 0; i < kFiles; ++i) {
    InodeID ino = 0;
    std::string name = "file_" + std::to_string(i);
    impl_->Create(kRoot, name, 0644, &ino, nullptr);
  }

  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(impl_->ReadDir(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), kFiles);

  // Verify no duplicate names.
  std::set<std::string> names;
  for (const auto& e : entries) {
    EXPECT_TRUE(names.insert(e.name).second) << "Duplicate entry: " << e.name;
    EXPECT_GT(e.ino, kRoot);
    EXPECT_EQ(e.type, DT_REG);
  }
}

// ────────────────────────────────────────────────────────────────
// ReadDir: not a directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirNotADirectory) {
  InodeID file_ino = 0;
  impl_->Create(kRoot, "regular", 0644, &file_ino, nullptr);

  std::vector<SwordFsEntry> entries;
  Status st = impl_->ReadDir(file_ino, &entries);
  EXPECT_TRUE(st.IsNotDirectory()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// ReadDir: directory with mixed file types
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirMixedTypes) {
  InodeID f_ino = 0, d_ino = 0;
  impl_->Create(kRoot, "file.txt", 0644, &f_ino, nullptr);
  impl_->MkDir(kRoot, "subdir", 0755, &d_ino, nullptr);

  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(impl_->ReadDir(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 2);

  for (const auto& e : entries) {
    if (e.name == "file.txt") {
      EXPECT_EQ(e.type, DT_REG);
    } else if (e.name == "subdir") {
      EXPECT_EQ(e.type, DT_DIR);
    } else {
      FAIL() << "Unexpected entry: " << e.name;
    }
  }
}

// ────────────────────────────────────────────────────────────────
// ReadDir: after rename, entries are consistent
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirAfterMove) {
  InodeID dir_a_ino = 0, dir_b_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &dir_a_ino, nullptr);
  impl_->MkDir(kRoot, "b", 0755, &dir_b_ino, nullptr);

  InodeID f_ino = 0;
  impl_->Create(dir_a_ino, "target", 0644, &f_ino, nullptr);

  // Move a/target → b/target
  impl_->Rename(dir_a_ino, "target", dir_b_ino, "target", 0);

  // Dir A should be empty
  std::vector<SwordFsEntry> entries_a;
  impl_->ReadDir(dir_a_ino, &entries_a);
  EXPECT_EQ(entries_a.size(), 0);

  // Dir B should have "target"
  std::vector<SwordFsEntry> entries_b;
  impl_->ReadDir(dir_b_ino, &entries_b);
  EXPECT_EQ(entries_b.size(), 1);
  EXPECT_EQ(entries_b[0].name, "target");
  EXPECT_EQ(entries_b[0].ino, f_ino);
}

// ────────────────────────────────────────────────────────────────
// ReadDir: after delete, entry is gone
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirAfterUnlink) {
  InodeID f_ino = 0;
  impl_->Create(kRoot, "to_delete", 0644, &f_ino, nullptr);

  impl_->Unlink(kRoot, "to_delete");

  std::vector<SwordFsEntry> entries;
  impl_->ReadDir(kRoot, &entries);
  EXPECT_EQ(entries.size(), 0);
}

// ────────────────────────────────────────────────────────────────
// ReadDir: large directory (batch-read simulation)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirLargeDirectory) {
  constexpr int kFiles = 200;
  for (int i = 0; i < kFiles; ++i) {
    InodeID ino = 0;
    impl_->Create(kRoot, "entry_" + std::to_string(i), 0644, &ino, nullptr);
  }

  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(impl_->ReadDir(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), kFiles);

  // All inodes should be unique.
  std::set<InodeID> inodes;
  for (const auto& e : entries) {
    inodes.insert(e.ino);
  }
  EXPECT_EQ(inodes.size(), kFiles) << "Duplicate inodes in ReadDir output";
}
