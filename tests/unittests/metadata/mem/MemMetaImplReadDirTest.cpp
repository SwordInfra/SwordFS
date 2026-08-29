// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for ReadDir / directory listing at the MemMetaImpl level.
// Validates the data layer that feeds into VfsImpl::Readdir/Readdirplus
// (the FUSE formatting fix is in PR #23).

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <set>

#include "TestMemMetaImpl.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::test::TestMemMetaImpl;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;

class MemMetaImplReadDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new TestMemMetaImpl();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override {
    delete impl_;
  }

  TestMemMetaImpl *impl_;
};

// ────────────────────────────────────────────────────────────────
// ReadDir: empty directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, ReadDirEmpty) {
  std::vector<SwordFsEntry> entries;
  Status st = impl_->ReadDir(kRoot, &entries);
  EXPECT_TRUE(st.ok()) << st.message();
  // Root starts empty: ReadDir still emits "." and "..".
  EXPECT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].name, ".");
  EXPECT_EQ(entries[0].ino, kRoot);
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_EQ(entries[1].ino, kRoot);  // root's parent is itself
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
  EXPECT_EQ(entries.size(), kFiles + 2);  // +2 for "." and ".."

  // Verify no duplicate names.
  std::set<std::string> names;
  for (const auto &e : entries) {
    EXPECT_TRUE(names.insert(e.name).second) << "Duplicate entry: " << e.name;
    if (e.name == "." || e.name == "..") {
      continue;
    }
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
  EXPECT_EQ(entries.size(), 4);  // 2 real + "." + ".."

  for (const auto &e : entries) {
    if (e.name == "file.txt") {
      EXPECT_EQ(e.type, DT_REG);
    } else if (e.name == "subdir") {
      EXPECT_EQ(e.type, DT_DIR);
    } else if (e.name == "." || e.name == "..") {
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
  impl_->Rename(dir_a_ino, "target", dir_b_ino, "target", RenameFlag::kNone);

  // Dir A should be empty (apart from "." and "..")
  std::vector<SwordFsEntry> entries_a;
  impl_->ReadDir(dir_a_ino, &entries_a);
  EXPECT_EQ(entries_a.size(), 2);

  // Dir B should have "target" + "." + ".."
  std::vector<SwordFsEntry> entries_b;
  impl_->ReadDir(dir_b_ino, &entries_b);
  EXPECT_EQ(entries_b.size(), 3);
  // Find the real entry (skip "." and "..")
  const SwordFsEntry *target_entry = nullptr;
  for (const auto &e : entries_b) {
    if (e.name != "." && e.name != "..") {
      target_entry = &e;
      break;
    }
  }
  ASSERT_NE(target_entry, nullptr);
  EXPECT_EQ(target_entry->name, "target");
  EXPECT_EQ(target_entry->ino, f_ino);
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
  EXPECT_EQ(entries.size(), 2);  // just "." and ".."
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
  EXPECT_EQ(entries.size(), kFiles + 2);

  // All real inodes should be unique.
  std::set<InodeID> inodes;
  for (const auto &e : entries) {
    if (e.name == "." || e.name == "..") {
      continue;
    }
    inodes.insert(e.ino);
  }
  EXPECT_EQ(inodes.size(), kFiles) << "Duplicate inodes in ReadDir output";
}
