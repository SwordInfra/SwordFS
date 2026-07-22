// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaStore::SwapEntries — the atomic directory-entry swap
// method added in PR #24 (RENAME_EXCHANGE support).

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <barrier>
#include <thread>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = FUSE_ROOT_ID;
static constexpr mode_t kRegFile = S_IFREG | 0644;
static constexpr mode_t kDir = S_IFDIR | 0755;

// ════════════════════════════════════════════════════════════════════
// MemMetaStoreSwapTest
// ════════════════════════════════════════════════════════════════════

class MemMetaStoreSwapTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }

  MemMetaStore* store_;
};

// ────────────────────────────────────────────────────────────────
// Basic cross-directory swap
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, CrossDirectorySwap) {
  SwordFsInode *dir_a = nullptr, *dir_b = nullptr;
  store_->AddEntry(kRoot, "a", kDir, 0, &dir_a);
  store_->AddEntry(kRoot, "b", kDir, 0, &dir_b);

  SwordFsInode *fa = nullptr, *fb = nullptr;
  store_->AddEntry(dir_a->ino, "x", kRegFile, 0, &fa);
  store_->AddEntry(dir_b->ino, "y", kRegFile, 0, &fb);

  Status st = store_->SwapEntries(dir_a->ino, "x", dir_b->ino, "y");
  EXPECT_TRUE(st.ok()) << st.message();

  // After swap: dir_a/x → fb, dir_b/y → fa
  SwordFsInode* found = nullptr;
  EXPECT_TRUE(store_->LookupEntry(dir_a->ino, "x", &found).ok());
  EXPECT_EQ(found->ino, fb->ino);

  EXPECT_TRUE(store_->LookupEntry(dir_b->ino, "y", &found).ok());
  EXPECT_EQ(found->ino, fa->ino);
}

// ────────────────────────────────────────────────────────────────
// Same-directory swap (different names)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SameDirectorySwapDifferentNames) {
  SwordFsInode *fa = nullptr, *fb = nullptr;
  store_->AddEntry(kRoot, "alpha", kRegFile, 0, &fa);
  store_->AddEntry(kRoot, "beta", kRegFile, 0, &fb);

  Status st = store_->SwapEntries(kRoot, "alpha", kRoot, "beta");
  EXPECT_TRUE(st.ok()) << st.message();

  // After swap: root/alpha → fb, root/beta → fa
  SwordFsInode* found = nullptr;
  EXPECT_TRUE(store_->LookupEntry(kRoot, "alpha", &found).ok());
  EXPECT_EQ(found->ino, fb->ino);

  EXPECT_TRUE(store_->LookupEntry(kRoot, "beta", &found).ok());
  EXPECT_EQ(found->ino, fa->ino);
}

// ────────────────────────────────────────────────────────────────
// Swap file and directory (should succeed at the store level —
// type checking is done at the MemMetaImpl layer)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapFileWithDirectory) {
  SwordFsInode *file = nullptr, *subdir = nullptr;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &file);
  store_->AddEntry(kRoot, "d", kDir, 0, &subdir);

  Status st = store_->SwapEntries(kRoot, "f", kRoot, "d");
  EXPECT_TRUE(st.ok()) << st.message();

  // Verify pointers were swapped correctly.
  SwordFsInode* found = nullptr;
  EXPECT_TRUE(store_->LookupEntry(kRoot, "f", &found).ok());
  EXPECT_TRUE(found->IsDir()) << "after swap, 'f' should be the directory";

  EXPECT_TRUE(store_->LookupEntry(kRoot, "d", &found).ok());
  EXPECT_FALSE(found->IsDir()) << "after swap, 'd' should be the file";
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing source A
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingSourceA) {
  SwordFsInode* fb = nullptr;
  store_->AddEntry(kRoot, "b", kRegFile, 0, &fb);

  Status st = store_->SwapEntries(kRoot, "no_such", kRoot, "b");
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing source B
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingSourceB) {
  SwordFsInode* fa = nullptr;
  store_->AddEntry(kRoot, "a", kRegFile, 0, &fa);

  Status st = store_->SwapEntries(kRoot, "a", kRoot, "no_such");
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing parent A
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingParentA) {
  SwordFsInode* fb = nullptr;
  store_->AddEntry(kRoot, "b", kRegFile, 0, &fb);

  Status st = store_->SwapEntries(9999, "a", kRoot, "b");
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing parent B
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingParentB) {
  SwordFsInode* fa = nullptr;
  store_->AddEntry(kRoot, "a", kRegFile, 0, &fa);

  Status st = store_->SwapEntries(kRoot, "a", 9999, "b");
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: concurrent swap (no double-free / data race)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, ConcurrentSwapConsistency) {
  // Set up two pairs to swap concurrently.
  SwordFsInode *fa1 = nullptr, *fa2 = nullptr;
  store_->AddEntry(kRoot, "a1", kRegFile, 0, &fa1);
  store_->AddEntry(kRoot, "a2", kRegFile, 0, &fa2);

  SwordFsInode *fb1 = nullptr, *fb2 = nullptr;
  store_->AddEntry(kRoot, "b1", kRegFile, 0, &fb1);
  store_->AddEntry(kRoot, "b2", kRegFile, 0, &fb2);

  std::atomic<int> ok_count{0};
  std::barrier gate(2);

  auto swapper = [&](int idx) {
    gate.arrive_and_wait();
    std::string src = "a" + std::to_string(idx + 1);
    std::string dst = "b" + std::to_string(idx + 1);
    Status st = store_->SwapEntries(kRoot, src, kRoot, dst);
    if (st.ok()) ok_count.fetch_add(1, std::memory_order_relaxed);
  };

  std::thread t1(swapper, 0);
  std::thread t2(swapper, 1);
  t1.join();
  t2.join();

  EXPECT_EQ(ok_count.load(), 2) << "Both swaps should succeed (different entries)";

  // Verify all entries still exist (no pointers lost).
  for (const auto& name : {"a1", "a2", "b1", "b2"}) {
    SwordFsInode* found = nullptr;
    EXPECT_TRUE(store_->LookupEntry(kRoot, name, &found).ok())
        << "Entry '" << name << "' lost after concurrent swaps";
  }
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: same entry (name) in the same directory — no-op
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapSameEntryNoOp) {
  SwordFsInode *f = nullptr;
  store_->AddEntry(kRoot, "only", kRegFile, 0, &f);

  Status st = store_->SwapEntries(kRoot, "only", kRoot, "only");
  EXPECT_TRUE(st.ok()) << st.message();

  // Entry should still point to the same inode.
  SwordFsInode* found = nullptr;
  EXPECT_TRUE(store_->LookupEntry(kRoot, "only", &found).ok());
  EXPECT_EQ(found->ino, f->ino);
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: cross-directory swap of directories
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapDirectoriesCrossDirectory) {
  SwordFsInode *dir_a = nullptr, *dir_b = nullptr;
  store_->AddEntry(kRoot, "a", kDir, 0, &dir_a);
  store_->AddEntry(kRoot, "b", kDir, 0, &dir_b);

  // Add children inside each
  store_->AddEntry(dir_a->ino, "child_a", kRegFile, 0, nullptr);
  store_->AddEntry(dir_b->ino, "child_b", kRegFile, 0, nullptr);

  Status st = store_->SwapEntries(kRoot, "a", kRoot, "b");
  EXPECT_TRUE(st.ok()) << st.message();

  // After swap: root/a → dir_b, root/b → dir_a
  SwordFsInode* found = nullptr;
  EXPECT_TRUE(store_->LookupEntry(kRoot, "a", &found).ok());
  EXPECT_EQ(found->ino, dir_b->ino);

  EXPECT_TRUE(store_->LookupEntry(kRoot, "b", &found).ok());
  EXPECT_EQ(found->ino, dir_a->ino);

  // Children should still be accessible via the swapped directories.
  // dir_b (now at "a") should have "child_b"
  std::vector<std::pair<std::string, SwordFsInode*>> entries;
  store_->ListEntries(dir_b->ino, &entries);
  EXPECT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].first, "child_b");
}
