// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

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

class MemMetaStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }

  MemMetaStore* store_;
};

// ────────────────────────────────────────────────────────────────
// Constructor & InodeCount
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ConstructorCreatesRoot) {
  EXPECT_EQ(store_->InodeCount(), 1);

  SwordFsInode* root = nullptr;
  EXPECT_TRUE(store_->LookupInode(kRoot, &root).ok());
  ASSERT_NE(root, nullptr);
  EXPECT_TRUE(root->IsDir());
  EXPECT_EQ(root->ino, kRoot);
}

// ────────────────────────────────────────────────────────────────
// LookupInode
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, LookupInodeNotFound) {
  SwordFsInode* out = nullptr;
  Status st = store_->LookupInode(999, &out);
  EXPECT_TRUE(st.IsNotFound());
  EXPECT_EQ(out, nullptr);
}

TEST_F(MemMetaStoreTest, LookupInodeWithNullOut) {
  EXPECT_TRUE(store_->LookupInode(kRoot, nullptr).ok());
}

// ────────────────────────────────────────────────────────────────
// AddEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, AddEntryCreatesFile) {
  SwordFsInode* child = nullptr;
  Status st = store_->AddEntry(kRoot, "hello.txt", kRegFile, 0, &child);

  EXPECT_TRUE(st.ok());
  ASSERT_NE(child, nullptr);
  EXPECT_FALSE(child->IsDir());
  EXPECT_GT(child->ino, 0);
  EXPECT_EQ(store_->InodeCount(), 2);
}

TEST_F(MemMetaStoreTest, AddEntryCreatesDirectory) {
  SwordFsInode* child = nullptr;
  Status st = store_->AddEntry(kRoot, "subdir", kDir, 0, &child);

  EXPECT_TRUE(st.ok());
  ASSERT_NE(child, nullptr);
  EXPECT_TRUE(child->IsDir());
}

TEST_F(MemMetaStoreTest, AddEntryAlreadyExists) {
  SwordFsInode* dummy = nullptr;
  store_->AddEntry(kRoot, "file", kRegFile, 0, &dummy);

  SwordFsInode* out = nullptr;
  Status st = store_->AddEntry(kRoot, "file", kRegFile, 0, &out);
  EXPECT_TRUE(st.IsAlreadyExists());
}

TEST_F(MemMetaStoreTest, AddEntryParentNotFound) {
  SwordFsInode* out = nullptr;
  Status st = store_->AddEntry(42, "orphan", kRegFile, 0, &out);
  EXPECT_TRUE(st.IsNotFound());
}

TEST_F(MemMetaStoreTest, AddEntryParentNotDirectory) {
  // Create a regular file first
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "regular", kRegFile, 0, &f);

  // Try to add a child under the regular file
  SwordFsInode* out = nullptr;
  Status st = store_->AddEntry(f->ino, "nested", kRegFile, 0, &out);
  EXPECT_TRUE(st.IsNotDirectory());
}

// ────────────────────────────────────────────────────────────────
// LookupEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, LookupEntryFound) {
  SwordFsInode* created = nullptr;
  store_->AddEntry(kRoot, "found", kRegFile, 0, &created);

  SwordFsInode* out = nullptr;
  Status st = store_->LookupEntry(kRoot, "found", &out);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(out, created);
}

TEST_F(MemMetaStoreTest, LookupEntryNotFound) {
  SwordFsInode* out = nullptr;
  Status st = store_->LookupEntry(kRoot, "nonexistent", &out);
  EXPECT_TRUE(st.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// MoveEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, MoveEntrySuccess) {
  // Create dir: root/sub/
  SwordFsInode* sub = nullptr;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);

  // Create file under root
  SwordFsInode* f1 = nullptr;
  store_->AddEntry(kRoot, "f1", kRegFile, 0, &f1);

  // Move root/f1 → root/sub/f1
  Status st = store_->MoveEntry(kRoot, "f1", sub->ino, "f1");
  EXPECT_TRUE(st.ok());

  // Old location is gone
  SwordFsInode* out = nullptr;
  EXPECT_TRUE(store_->LookupEntry(kRoot, "f1", &out).IsNotFound());

  // New location has it
  SwordFsInode* moved = nullptr;
  EXPECT_TRUE(store_->LookupEntry(sub->ino, "f1", &moved).ok());
  EXPECT_EQ(moved, f1);  // same pointer — re-linked, not copied
}

TEST_F(MemMetaStoreTest, MoveEntryOldParentNotFound) {
  SwordFsInode* sub = nullptr;
  store_->AddEntry(kRoot, "dst", kDir, 0, &sub);

  Status st = store_->MoveEntry(999, "f", sub->ino, "f");
  EXPECT_TRUE(st.IsNotFound());
}

TEST_F(MemMetaStoreTest, MoveEntryNewParentNotFound) {
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);

  Status st = store_->MoveEntry(kRoot, "f", 999, "f");
  EXPECT_TRUE(st.IsNotFound());
}

TEST_F(MemMetaStoreTest, MoveEntryTargetExists) {
  SwordFsInode* d1 = nullptr;
  store_->AddEntry(kRoot, "d1", kDir, 0, &d1);

  SwordFsInode* d2 = nullptr;
  store_->AddEntry(kRoot, "d2", kDir, 0, &d2);

  // Both have a "f" entry
  store_->AddEntry(d1->ino, "f", kRegFile, 0, nullptr);
  store_->AddEntry(d2->ino, "f", kRegFile, 0, nullptr);

  Status st = store_->MoveEntry(d1->ino, "f", d2->ino, "f");
  EXPECT_TRUE(st.IsAlreadyExists());
}

// ────────────────────────────────────────────────────────────────
// RemoveEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, RemoveEntryDeletesInode) {
  SwordFsInode* f = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  InodeID ino = f->ino;

  EXPECT_EQ(store_->InodeCount(), 2);  // root + f
  EXPECT_TRUE(store_->RemoveEntry(kRoot, "f").ok());

  // Entry gone from directory
  EXPECT_TRUE(store_->LookupEntry(kRoot, "f", nullptr).IsNotFound());
  // Inode freed
  EXPECT_EQ(store_->InodeCount(), 1);
  EXPECT_TRUE(store_->LookupInode(ino, nullptr).IsNotFound());
}

TEST_F(MemMetaStoreTest, RemoveEntryIdempotent) {
  EXPECT_TRUE(store_->RemoveEntry(kRoot, "nonexistent").ok());
  EXPECT_EQ(store_->InodeCount(), 1);
}

TEST_F(MemMetaStoreTest, RemoveEntryNonEmptyDirectory) {
  SwordFsInode* sub = nullptr;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);

  // Add a file inside sub
  store_->AddEntry(sub->ino, "f", kRegFile, 0, nullptr);

  Status st = store_->RemoveEntry(kRoot, "sub");
  EXPECT_TRUE(st.IsBusy());
  EXPECT_EQ(store_->InodeCount(), 3);  // root + sub + f
}

TEST_F(MemMetaStoreTest, RemoveEntryEmptyDirectory) {
  SwordFsInode* sub = nullptr;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);
  InodeID sub_ino = sub->ino;  // save before RemoveEntry frees the pointer

  EXPECT_TRUE(store_->RemoveEntry(kRoot, "sub").ok());
  // Both sub and its dir entry table freed
  EXPECT_TRUE(store_->LookupInode(sub_ino, nullptr).IsNotFound());
  EXPECT_EQ(store_->InodeCount(), 1);
}

// ────────────────────────────────────────────────────────────────
// ListEntries
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ListEntriesSuccess) {
  store_->AddEntry(kRoot, "a.txt", kRegFile, 0, nullptr);
  store_->AddEntry(kRoot, "b.txt", kRegFile, 0, nullptr);
  store_->AddEntry(kRoot, "c", kDir, 0, nullptr);

  std::vector<std::pair<std::string, SwordFsInode*>> entries;
  Status st = store_->ListEntries(kRoot, &entries);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(entries.size(), 3);
}

TEST_F(MemMetaStoreTest, ListEntriesEmptyDir) {
  std::vector<std::pair<std::string, SwordFsInode*>> entries;
  EXPECT_TRUE(store_->ListEntries(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 0);
}

TEST_F(MemMetaStoreTest, ListEntriesNotFound) {
  std::vector<std::pair<std::string, SwordFsInode*>> entries;
  Status st = store_->ListEntries(42, &entries);
  EXPECT_TRUE(st.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// IsDescendantOf
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, IsDescendantOfDirectChild) {
  SwordFsInode* sub = nullptr;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);

  EXPECT_TRUE(store_->IsDescendantOf(kRoot, sub->ino));
}

TEST_F(MemMetaStoreTest, IsDescendantOfGrandchild) {
  SwordFsInode* sub = nullptr;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);
  SwordFsInode* leaf = nullptr;
  store_->AddEntry(sub->ino, "nested", kRegFile, 0, &leaf);

  EXPECT_TRUE(store_->IsDescendantOf(kRoot, leaf->ino));
  EXPECT_TRUE(store_->IsDescendantOf(sub->ino, leaf->ino));
}

TEST_F(MemMetaStoreTest, IsDescendantOfNotDescendant) {
  SwordFsInode* a = nullptr;
  store_->AddEntry(kRoot, "a", kDir, 0, &a);
  SwordFsInode* b = nullptr;
  store_->AddEntry(kRoot, "b", kDir, 0, &b);
  SwordFsInode* leaf = nullptr;
  store_->AddEntry(b->ino, "leaf", kRegFile, 0, &leaf);

  // leaf is under b, not under a
  EXPECT_FALSE(store_->IsDescendantOf(a->ino, leaf->ino));
}

TEST_F(MemMetaStoreTest, IsDescendantOfSelf) {
  EXPECT_FALSE(store_->IsDescendantOf(kRoot, kRoot));
}
