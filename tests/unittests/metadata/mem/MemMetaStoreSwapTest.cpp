// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaTxn::SwapEntries — the atomic directory-entry swap
// primitive backing RENAME_EXCHANGE (PR #24).  Like the production
// code, the tests go through MemMetaStore::Transact() exclusively.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::FileType;
using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::MemMetaTxn;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
constexpr FileType kRegFileType = FileType::kRegular;
constexpr mode_t kRegFilePermissions = 0644;
constexpr FileType kDirType = FileType::kDirectory;
constexpr mode_t kDirPermissions = 0755;

// ════════════════════════════════════════════════════════════════════
// MemMetaStoreSwapTest
// ════════════════════════════════════════════════════════════════════

class MemMetaStoreSwapTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }

  MemMetaStore *store_;
};

// ────────────────────────────────────────────────────────────────
// Basic cross-directory swap
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, CrossDirectorySwap) {
  SwordFsInode dir_a, dir_b;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a", kDirType, kDirPermissions, &dir_a);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b", kDirType, kDirPermissions, &dir_b);
  });

  SwordFsInode fa, fb;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_a.ino, "x", kRegFileType, kRegFilePermissions, &fa);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_b.ino, "y", kRegFileType, kRegFilePermissions, &fb);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(dir_a.ino, "x", dir_b.ino, "y");
  });
  EXPECT_TRUE(status.ok()) << status.message();

  // After swap: dir_a/x → fb, dir_b/y → fa
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(dir_a.ino, "x", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, fb.ino);

  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(dir_b.ino, "y", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, fa.ino);
}

// ────────────────────────────────────────────────────────────────
// Same-directory swap (different names)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SameDirectorySwapDifferentNames) {
  SwordFsInode fa, fb;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "alpha", kRegFileType, kRegFilePermissions, &fa);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "beta", kRegFileType, kRegFilePermissions, &fb);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "alpha", kRoot, "beta");
  });
  EXPECT_TRUE(status.ok()) << status.message();

  // After swap: root/alpha → fb, root/beta → fa
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "alpha", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, fb.ino);

  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "beta", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, fa.ino);
}

// ────────────────────────────────────────────────────────────────
// Swap file and directory (should succeed at the store level —
// type checking is done at the MemMetaImpl layer)
// ────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapFileWithDirectory) {
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "f", kRegFileType, kRegFilePermissions, nullptr);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "d", kDirType, kDirPermissions, nullptr);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "f", kRoot, "d");
  });
  EXPECT_TRUE(status.ok()) << status.message();

  // Verify entries were swapped correctly.
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "f", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(found.IsDir()) << "after swap, 'f' should be the directory";

  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "d", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(found.IsDir()) << "after swap, 'd' should be the file";
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing source A
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingSourceA) {
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b", kRegFileType, kRegFilePermissions, nullptr);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "no_such", kRoot, "b");
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing source B
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingSourceB) {
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a", kRegFileType, kRegFilePermissions, nullptr);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "a", kRoot, "no_such");
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing parent A
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingParentA) {
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b", kRegFileType, kRegFilePermissions, nullptr);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(9999, "a", kRoot, "b");
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: missing parent B
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapMissingParentB) {
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a", kRegFileType, kRegFilePermissions, nullptr);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "a", 9999, "b");
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: concurrent swap (no double-free / data race)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, ConcurrentSwapConsistency) {
  // Set up two pairs to swap concurrently.
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a1", kRegFileType, kRegFilePermissions, nullptr);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a2", kRegFileType, kRegFilePermissions, nullptr);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b1", kRegFileType, kRegFilePermissions, nullptr);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b2", kRegFileType, kRegFilePermissions, nullptr);
  });

  std::atomic<int> ok_count{0};
  std::barrier gate(2);

  auto swapper = [&](int idx) {
    gate.arrive_and_wait();
    std::string src = "a" + std::to_string(idx + 1);
    std::string dst = "b" + std::to_string(idx + 1);
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.SwapEntries(kRoot, src, kRoot, dst);
    });
    if (status.ok()) {
      ok_count.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread t1(swapper, 0);
  std::thread t2(swapper, 1);
  t1.join();
  t2.join();

  EXPECT_EQ(ok_count.load(), 2)
      << "Both swaps should succeed (different entries)";

  // Verify all entries still exist (no entries lost).
  for (const auto &name : {"a1", "a2", "b1", "b2"}) {
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.LookupEntry(kRoot, name, nullptr);
    });
    EXPECT_TRUE(status.ok()) << "Entry '" << name << "' lost after concurrent swaps";
  }
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: same entry (name) in the same directory — no-op
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapSameEntryNoOp) {
  SwordFsInode f;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "only", kRegFileType, kRegFilePermissions, &f);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "only", kRoot, "only");
  });
  EXPECT_TRUE(status.ok()) << status.message();

  // Entry should still point to the same inode.
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "only", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, f.ino);
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: cross-directory swap of directories
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreSwapTest, SwapDirectoriesCrossDirectory) {
  SwordFsInode dir_a, dir_b;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a", kDirType, kDirPermissions, &dir_a);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b", kDirType, kDirPermissions, &dir_b);
  });

  // Add children inside each
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_a.ino, "child_a", kRegFileType, kRegFilePermissions, nullptr);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_b.ino, "child_b", kRegFileType, kRegFilePermissions, nullptr);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "a", kRoot, "b");
  });
  EXPECT_TRUE(status.ok()) << status.message();

  // After swap: root/a → dir_b, root/b → dir_a
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "a", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, dir_b.ino);

  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "b", &found);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(found.ino, dir_a.ino);

  // Children should still be accessible via the swapped directories.
  // dir_b (now at "a") should have "child_b"
  std::vector<SwordFsEntry> entries;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.ListEntries(dir_b.ino, &entries);
  });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(entries.size(), 3);  // "child_b" + "." + ".."
  EXPECT_EQ(entries[0].name, ".");
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_EQ(entries[2].name, "child_b");
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: swap across different parents updates parent_ino (META-04)
// ────────────────────────────────────────────────────────────────
// The previous "cross-directory" test above actually swaps within the
// same parent (kRoot/kRoot).  This test swaps two directories that live
// under DIFFERENT parents and verifies each inode's parent_ino follows
// the swap, so ListEntries synthesizes the correct ".." entry.

TEST_F(MemMetaStoreSwapTest, SwapAcrossDifferentParentsUpdatesParentIno) {
  SwordFsInode p1, p2;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "p1", kDirType, kDirPermissions, &p1);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "p2", kDirType, kDirPermissions, &p2);
  });

  SwordFsInode dir_a, dir_b;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(p1.ino, "a", kDirType, kDirPermissions, &dir_a);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(p2.ino, "b", kDirType, kDirPermissions, &dir_b);
  });

  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(p1.ino, "a", p2.ino, "b");
  });
  ASSERT_TRUE(status.ok()) << status.message();

  // After the swap: p1/a -> dir_b, p2/b -> dir_a.  Snapshots taken
  // before the swap are stale by definition, so re-read the inodes.
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupInode(dir_a.ino, &found);
  });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(found.parent_ino, p2.ino);
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupInode(dir_b.ino, &found);
  });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(found.parent_ino, p1.ino);

  // The synthetic ".." entry must point at the new parent.
  std::vector<SwordFsEntry> entries;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.ListEntries(dir_a.ino, &entries);
  });
  ASSERT_TRUE(status.ok());
  ASSERT_GE(entries.size(), 2);
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_EQ(entries[1].ino, p2.ino);

  entries.clear();
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.ListEntries(dir_b.ino, &entries);
  });
  ASSERT_TRUE(status.ok());
  ASSERT_GE(entries.size(), 2);
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_EQ(entries[1].ino, p1.ino);
}

// ────────────────────────────────────────────────────────────────
// SwapEntry: a directory can never land inside its own subtree
// ────────────────────────────────────────────────────────────────
// RENAME_EXCHANGE must reject cycles just like a plain rename does —
// in both directions.

TEST_F(MemMetaStoreSwapTest, SwapDirectoryIntoOwnSubtreeFails) {
  // Build root/b/x/a: dir_a is a descendant of dir_b.
  SwordFsInode dir_b;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b", kDirType, kDirPermissions, &dir_b);
  });
  SwordFsInode dir_x;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_b.ino, "x", kDirType, kDirPermissions, &dir_x);
  });
  SwordFsInode dir_a;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_x.ino, "a", kDirType, kDirPermissions, &dir_a);
  });

  // Swapping dir_b into dir_a's slot puts dir_b inside its own subtree.
  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(kRoot, "b", dir_x.ino, "a");
  });
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // Same cycle from the other direction.
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.SwapEntries(dir_x.ino, "a", kRoot, "b");
  });
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // The tree must be left untouched.
  SwordFsInode found;
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(kRoot, "b", &found);
  });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(found.ino, dir_b.ino);
  status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(dir_x.ino, "a", &found);
  });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(found.ino, dir_a.ino);
}
