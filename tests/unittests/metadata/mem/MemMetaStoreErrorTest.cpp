// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// MemMetaStore error path and edge case tests.

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = FUSE_ROOT_ID;
static constexpr mode_t kRegFile = S_IFREG | 0644;
static constexpr mode_t kDir = S_IFDIR | 0755;

class MemMetaStoreErrorTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }
  MemMetaStore* store_;
};

// ────────────────────────────────────────────────────────────────
// InodeCount
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreErrorTest, InodeCountAfterInsertAndRemove) {
  EXPECT_EQ(store_->InodeCount(), 1);  // only root
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "a", kRegFile, 0, &f);
  EXPECT_EQ(store_->InodeCount(), 2);
  store_->AddEntry(kRoot, "b", kRegFile, 0, nullptr);
  EXPECT_EQ(store_->InodeCount(), 3);
  store_->RemoveEntry(kRoot, "a");
  EXPECT_EQ(store_->InodeCount(), 2);
}

// ────────────────────────────────────────────────────────────────
// RemoveEntry idempotent + edge
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreErrorTest, RemoveEntryDoubleRemove) {
  store_->AddEntry(kRoot, "f", kRegFile, 0, nullptr);
  EXPECT_TRUE(store_->RemoveEntry(kRoot, "f").ok());
  EXPECT_TRUE(store_->RemoveEntry(kRoot, "f").ok());  // idempotent
  EXPECT_EQ(store_->InodeCount(), 1);
}

TEST_F(MemMetaStoreErrorTest, RemoveEntryParentNotDirectory) {
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  // f is a regular file, try to remove from it as parent
  Status st = store_->RemoveEntry(f->ino, "anything");
  // MemMetaStore RemoveEntry doesn't check parent type
  EXPECT_TRUE(st.ok()) << "RemoveEntry from non-dir parent is ok (entry not found)";
}

// ────────────────────────────────────────────────────────────────
// MoveEntry error paths
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreErrorTest, MoveEntrySourceNotFound) {
  SwordFsInode* dst = nullptr;
  store_->AddEntry(kRoot, "dst", kDir, 0, &dst);
  Status st = store_->MoveEntry(kRoot, "nonexistent", dst->ino, "newname");
  EXPECT_TRUE(st.IsNotFound());
}

TEST_F(MemMetaStoreErrorTest, MoveEntryDestAlreadyExists) {
  SwordFsInode *src = nullptr, *dst = nullptr;
  store_->AddEntry(kRoot, "src", kDir, 0, &src);
  store_->AddEntry(kRoot, "dst", kDir, 0, &dst);
  store_->AddEntry(src->ino, "f", kRegFile, 0, nullptr);
  store_->AddEntry(dst->ino, "f", kRegFile, 0, nullptr);
  Status st = store_->MoveEntry(src->ino, "f", dst->ino, "f");
  EXPECT_TRUE(st.IsAlreadyExists());
}

TEST_F(MemMetaStoreErrorTest, MoveEntryOldParentNotDirectory) {
  SwordFsInode *f = nullptr, *dst = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  store_->AddEntry(kRoot, "dst", kDir, 0, &dst);
  Status st = store_->MoveEntry(f->ino, "anything", dst->ino, "newname");
  EXPECT_TRUE(st.IsNotDirectory());
}

// ────────────────────────────────────────────────────────────────
// AddEntry error paths
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreErrorTest, AddEntryParentNotFound) {
  SwordFsInode* out = nullptr;
  Status st = store_->AddEntry(99999, "orphan", kRegFile, 0, &out);
  EXPECT_TRUE(st.IsNotFound());
}

TEST_F(MemMetaStoreErrorTest, AddEntryAlreadyExists) {
  store_->AddEntry(kRoot, "dup", kRegFile, 0, nullptr);
  SwordFsInode* out = nullptr;
  Status st = store_->AddEntry(kRoot, "dup", kRegFile, 0, &out);
  EXPECT_TRUE(st.IsAlreadyExists());
}

// ────────────────────────────────────────────────────────────────
// LookupEntry error paths
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreErrorTest, LookupEntryParentNotDirectory) {
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  SwordFsInode* out = nullptr;
  Status st = store_->LookupEntry(f->ino, "anything", &out);
  EXPECT_TRUE(st.IsNotDirectory());
}

TEST_F(MemMetaStoreErrorTest, LookupEntryNullOutput) {
  store_->AddEntry(kRoot, "f", kRegFile, 0, nullptr);
  EXPECT_TRUE(store_->LookupEntry(kRoot, "f", nullptr).ok());
}

// ────────────────────────────────────────────────────────────────
// ListEntries on non-directory
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreErrorTest, ListEntriesOnFile) {
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  std::vector<std::pair<std::string, SwordFsInode*>> entries;
  Status st = store_->ListEntries(f->ino, &entries);
  // Current behavior: doesn't check if inode is a directory
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(entries.size(), 0);
}
