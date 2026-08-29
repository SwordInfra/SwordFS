// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for directory iteration at the MemMetaImpl level.
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

using swordfs::metadata::DirIteratorPtr;
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

  Status CollectEntries(InodeID ino, std::vector<SwordFsEntry> *entries) {
    DirIteratorPtr iterator;
    auto status = impl_->OpenDir(ino, &iterator);
    if (!status.ok()) {
      return status;
    }
    for (uint64_t cookie = 0;;) {
      SwordFsEntry entry;
      uint64_t next_cookie = 0;
      status = iterator->Next(cookie, &entry, &next_cookie);
      if (status.IsEndOfDirectory()) {
        return Status::OK();
      }
      if (!status.ok()) {
        return status;
      }
      entries->push_back(std::move(entry));
      cookie = next_cookie;
    }
  }

  TestMemMetaImpl *impl_;
};

// ────────────────────────────────────────────────────────────────
// Empty directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirEmpty) {
  std::vector<SwordFsEntry> entries;
  Status st = CollectEntries(kRoot, &entries);
  EXPECT_TRUE(st.ok()) << st.message();
  // Root starts empty: iteration still emits "." and "..".
  EXPECT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].name, ".");
  EXPECT_EQ(entries[0].ino, kRoot);
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_EQ(entries[1].ino, kRoot);  // root's parent is itself
}

// ────────────────────────────────────────────────────────────────
// Non-empty directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirWithEntries) {
  constexpr int kFiles = 10;
  for (int i = 0; i < kFiles; ++i) {
    InodeID ino = 0;
    std::string name = "file_" + std::to_string(i);
    impl_->Create(kRoot, name, 0644, &ino, nullptr);
  }

  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(CollectEntries(kRoot, &entries).ok());
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
// OpenDir: caller-owned iterator can be continued
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirIteratorSupportsPeekNextAndSeek) {
  InodeID first_ino = 0, second_ino = 0;
  impl_->Create(kRoot, "first", 0644, &first_ino, nullptr);
  impl_->Create(kRoot, "second", 0644, &second_ino, nullptr);

  DirIteratorPtr iterator;
  ASSERT_TRUE(impl_->OpenDir(kRoot, &iterator).ok());
  ASSERT_NE(iterator, nullptr);

  SwordFsEntry entry;
  uint64_t next_offset = 0;
  ASSERT_TRUE(iterator->Peek(0, &entry).ok());
  EXPECT_EQ(entry.name, ".");
  ASSERT_TRUE(iterator->Next(0, &entry, &next_offset).ok());
  EXPECT_EQ(entry.name, ".");
  EXPECT_EQ(next_offset, 1);

  ASSERT_TRUE(iterator->Next(1, &entry, &next_offset).ok());
  EXPECT_EQ(entry.name, "..");
  EXPECT_EQ(next_offset, 2);

  ASSERT_TRUE(iterator->Next(2, &entry, &next_offset).ok());
  EXPECT_EQ(next_offset, 3);
  const std::string first_entry_name = entry.name;

  // A previously returned cookie can be used to seek backwards.
  ASSERT_TRUE(iterator->Peek(2, &entry).ok());
  EXPECT_EQ(entry.name, first_entry_name);
  ASSERT_TRUE(iterator->Next(2, &entry, &next_offset).ok());
  EXPECT_EQ(entry.name, first_entry_name);
  EXPECT_EQ(next_offset, 3);
}

TEST_F(MemMetaImplReadDirTest, OpenDirRejectsNonDirectory) {
  InodeID file_ino = 0;
  impl_->Create(kRoot, "regular", 0644, &file_ino, nullptr);

  DirIteratorPtr iterator;
  EXPECT_EQ(impl_->OpenDir(file_ino, &iterator).code(), Status::kNotDirectory);
}

// ────────────────────────────────────────────────────────────────
// ReadDir: not a directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirNotADirectory) {
  InodeID file_ino = 0;
  impl_->Create(kRoot, "regular", 0644, &file_ino, nullptr);

  DirIteratorPtr iterator;
  Status st = impl_->OpenDir(file_ino, &iterator);
  EXPECT_TRUE(st.IsNotDirectory()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// Directory with mixed file types
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirMixedTypes) {
  InodeID f_ino = 0, d_ino = 0;
  impl_->Create(kRoot, "file.txt", 0644, &f_ino, nullptr);
  impl_->MkDir(kRoot, "subdir", 0755, &d_ino, nullptr);

  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(CollectEntries(kRoot, &entries).ok());
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
// After rename, entries are consistent
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirAfterMove) {
  InodeID dir_a_ino = 0, dir_b_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &dir_a_ino, nullptr);
  impl_->MkDir(kRoot, "b", 0755, &dir_b_ino, nullptr);

  InodeID f_ino = 0;
  impl_->Create(dir_a_ino, "target", 0644, &f_ino, nullptr);

  // Move a/target → b/target
  impl_->Rename(dir_a_ino, "target", dir_b_ino, "target", RenameFlag::kNone);

  // Dir A should be empty (apart from "." and "..")
  std::vector<SwordFsEntry> entries_a;
  ASSERT_TRUE(CollectEntries(dir_a_ino, &entries_a).ok());
  EXPECT_EQ(entries_a.size(), 2);

  // Dir B should have "target" + "." + ".."
  std::vector<SwordFsEntry> entries_b;
  ASSERT_TRUE(CollectEntries(dir_b_ino, &entries_b).ok());
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
// After delete, entry is gone
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirAfterUnlink) {
  InodeID f_ino = 0;
  impl_->Create(kRoot, "to_delete", 0644, &f_ino, nullptr);

  impl_->Unlink(kRoot, "to_delete");

  std::vector<SwordFsEntry> entries;
  ASSERT_TRUE(CollectEntries(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 2);  // just "." and ".."
}

// ────────────────────────────────────────────────────────────────
// Large directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplReadDirTest, OpenDirLargeDirectory) {
  constexpr int kFiles = 200;
  for (int i = 0; i < kFiles; ++i) {
    InodeID ino = 0;
    impl_->Create(kRoot, "entry_" + std::to_string(i), 0644, &ino, nullptr);
  }

  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(CollectEntries(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), kFiles + 2);

  // All real inodes should be unique.
  std::set<InodeID> inodes;
  for (const auto &e : entries) {
    if (e.name == "." || e.name == "..") {
      continue;
    }
    inodes.insert(e.ino);
  }
  EXPECT_EQ(inodes.size(), kFiles) << "Duplicate inodes in directory iteration output";
}
