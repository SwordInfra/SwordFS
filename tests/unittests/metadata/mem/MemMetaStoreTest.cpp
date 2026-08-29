// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaStore.  Tests use the store exactly the way
// production code (MemMetaImpl) does: every operation — including
// single primitives — runs inside a Transact() script.  Read methods
// hand out snapshot COPIES of SwordFsInode, never pointers into
// store-owned memory.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::MemMetaTxn;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
static constexpr mode_t kRegFile = S_IFREG | 0644;
static constexpr mode_t kDir = S_IFDIR | 0755;

class MemMetaStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    store_ = new MemMetaStore();
  }
  void TearDown() override {
    delete store_;
  }

  // Single-primitive transaction convenience wrappers, so individual
  // assertions read like production call sites.
  Status Add(InodeID parent_ino, std::string_view name, mode_t mode, SwordFsInode *out = nullptr) {
    return store_->Transact([&](MemMetaTxn &txn) { return txn.AddEntry(parent_ino, name, mode, out); });
  }
  Status Lookup(InodeID ino, SwordFsInode *out = nullptr) {
    return store_->Transact([&](MemMetaTxn &txn) { return txn.LookupInode(ino, out); });
  }
  Status LookupChild(InodeID parent_ino, std::string_view name, SwordFsInode *out = nullptr) {
    return store_->Transact([&](MemMetaTxn &txn) { return txn.LookupEntry(parent_ino, name, out); });
  }

  MemMetaStore *store_;
};

// ────────────────────────────────────────────────────────────────
// Constructor & InodeCount
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ConstructorCreatesRoot) {
  size_t count = store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(count, 1);

  SwordFsInode root;
  EXPECT_TRUE(Lookup(kRoot, &root).ok());
  EXPECT_TRUE(root.IsDir());
  EXPECT_EQ(root.ino, kRoot);
}

// ────────────────────────────────────────────────────────────────
// LookupInode
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, LookupInodeNotFound) {
  SwordFsInode out;
  Status status = Lookup(999, &out);
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, LookupInodeWithNullOut) {
  EXPECT_TRUE(Lookup(kRoot).ok());
}

// ────────────────────────────────────────────────────────────────
// AddEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, AddEntryCreatesFile) {
  SwordFsInode child;
  Status status = Add(kRoot, "hello.txt", kRegFile, &child);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(child.IsDir());
  EXPECT_GT(child.ino, 0);

  size_t count = store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(count, 2);
}

TEST_F(MemMetaStoreTest, AddEntryCreatesDirectory) {
  SwordFsInode child;
  Status status = Add(kRoot, "subdir", kDir, &child);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(child.IsDir());
}

TEST_F(MemMetaStoreTest, AddEntryAlreadyExists) {
  Add(kRoot, "file", kRegFile);

  Status status = Add(kRoot, "file", kRegFile);
  EXPECT_TRUE(status.IsAlreadyExists());
}

TEST_F(MemMetaStoreTest, AddEntryParentNotFound) {
  Status status = Add(42, "orphan", kRegFile);
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, AddEntryParentNotDirectory) {
  // Create a regular file first
  SwordFsInode f;
  Add(kRoot, "regular", kRegFile, &f);

  // Try to add a child under the regular file
  Status status = Add(f.ino, "nested", kRegFile);
  EXPECT_TRUE(status.IsNotDirectory());
}

// ────────────────────────────────────────────────────────────────
// LookupEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, LookupEntryFound) {
  SwordFsInode created;
  Add(kRoot, "found", kRegFile, &created);

  SwordFsInode out;
  Status status = LookupChild(kRoot, "found", &out);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(out.ino, created.ino);
}

TEST_F(MemMetaStoreTest, LookupEntryNotFound) {
  Status status = LookupChild(kRoot, "nonexistent");
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// MoveEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, MoveEntrySuccess) {
  // Create dir: root/sub/
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);

  // Create file under root
  SwordFsInode f1;
  Add(kRoot, "f1", kRegFile, &f1);

  // Move root/f1 → root/sub/f1
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(kRoot, "f1", sub.ino, "f1", false); });
  EXPECT_TRUE(status.ok());

  // Old location is gone
  EXPECT_TRUE(LookupChild(kRoot, "f1").IsNotFound());

  // New location has it, with the same ino — re-linked, not copied
  SwordFsInode moved;
  EXPECT_TRUE(LookupChild(sub.ino, "f1", &moved).ok());
  EXPECT_EQ(moved.ino, f1.ino);
  // ... and its parent pointer followed the move
  EXPECT_EQ(moved.parent_ino, sub.ino);
}

TEST_F(MemMetaStoreTest, MoveEntryOldParentNotFound) {
  SwordFsInode sub;
  Add(kRoot, "dst", kDir, &sub);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(999, "f", sub.ino, "f", false); });
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, MoveEntryNewParentNotFound) {
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(kRoot, "f", 999, "f", false); });
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, MoveEntryTargetExists) {
  SwordFsInode d1;
  Add(kRoot, "d1", kDir, &d1);

  SwordFsInode d2;
  Add(kRoot, "d2", kDir, &d2);

  // Both have a "f" entry
  Add(d1.ino, "f", kRegFile);
  Add(d2.ino, "f", kRegFile);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(d1.ino, "f", d2.ino, "f", false); });
  EXPECT_TRUE(status.IsAlreadyExists());
}

// ────────────────────────────────────────────────────────────────
// Unlink
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, UnlinkOnlyRemovesDirectoryEntry) {
  // Unlink only detaches the directory entry and decrements nlink.
  // The inode survives until the caller (VfsImpl::Unlink or
  // InodeHandle::Close) calls ReclaimData. Callers that want immediate
  // cleanup of a single-name unlink invoke ReclaimData themselves.
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;

  auto count = [&] { return store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); }); };

  EXPECT_EQ(count(), 2);  // root + f
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "f"); });
  EXPECT_TRUE(status.ok());

  // Entry gone from directory
  EXPECT_TRUE(LookupChild(kRoot, "f").IsNotFound());
  // Inode and its metadata stay alive (nlink == 0).
  EXPECT_EQ(count(), 2);
  EXPECT_TRUE(Lookup(ino).ok());

  // Caller follows up with ReclaimInode to free the orphaned inode.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.ReclaimInode(ino); });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(count(), 1);
  EXPECT_TRUE(Lookup(ino).IsNotFound());
}

TEST_F(MemMetaStoreTest, UnlinkMissingEntryReturnsNotFound) {
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "nonexistent"); });
  EXPECT_TRUE(status.IsNotFound());
  size_t count = store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(count, 1);
}

TEST_F(MemMetaStoreTest, UnlinkNonEmptyDirectory) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);

  // Add a file inside sub
  Add(sub.ino, "f", kRegFile);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "sub"); });
  EXPECT_TRUE(status.IsNotEmpty());
  size_t count = store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(count, 3);  // root + sub + f
}

TEST_F(MemMetaStoreTest, UnlinkEmptyDirectory) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);
  InodeID sub_ino = sub.ino;

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "sub"); });
  EXPECT_TRUE(status.ok());
  // Both sub and its dir entry table freed
  EXPECT_TRUE(Lookup(sub_ino).IsNotFound());
  size_t count = store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(count, 1);
}

// ────────────────────────────────────────────────────────────────
// ListEntries
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ListEntriesSuccess) {
  Add(kRoot, "a.txt", kRegFile);
  Add(kRoot, "b.txt", kRegFile);
  Add(kRoot, "c", kDir);

  std::vector<SwordFsEntry> entries;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(kRoot, &entries); });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(entries.size(), 5);  // 3 real + "." + ".."
}

TEST_F(MemMetaStoreTest, ListEntriesEmptyDir) {
  std::vector<SwordFsEntry> entries;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(kRoot, &entries); });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(entries.size(), 2);  // just "." and ".."
}

TEST_F(MemMetaStoreTest, ListEntriesNotFound) {
  std::vector<SwordFsEntry> entries;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(42, &entries); });
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// IsDescendantOf
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, IsDescendantOfDirectChild) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);

  bool result = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(kRoot, sub.ino); });
  EXPECT_TRUE(result);
}

TEST_F(MemMetaStoreTest, IsDescendantOfGrandchild) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);
  SwordFsInode leaf;
  Add(sub.ino, "nested", kRegFile, &leaf);

  bool from_root = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(kRoot, leaf.ino); });
  bool from_sub = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(sub.ino, leaf.ino); });
  EXPECT_TRUE(from_root);
  EXPECT_TRUE(from_sub);
}

TEST_F(MemMetaStoreTest, IsDescendantOfNotDescendant) {
  SwordFsInode a;
  Add(kRoot, "a", kDir, &a);
  SwordFsInode b;
  Add(kRoot, "b", kDir, &b);
  SwordFsInode leaf;
  Add(b.ino, "leaf", kRegFile, &leaf);

  // leaf is under b, not under a
  bool result = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(a.ino, leaf.ino); });
  EXPECT_FALSE(result);
}

TEST_F(MemMetaStoreTest, IsDescendantOfSelf) {
  bool result = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(kRoot, kRoot); });
  EXPECT_FALSE(result);
}

// ────────────────────────────────────────────────────────────────
// Chunk metadata: AddChunk / FindChunk
// ────────────────────────────────────────────────────────────────

namespace {
SwordFsChunk MakeChunk(ChunkIndex index, uint64_t start_offset, size_t size) {
  SwordFsChunk chunk;
  chunk.index = index;
  chunk.start_offset = start_offset;
  chunk.key = "key";
  chunk.size = size;
  return chunk;
}
}  // namespace

TEST_F(MemMetaStoreTest, AddChunkAndFindChunk) {
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.AddChunk(42, MakeChunk(0, 0, 100)); });
  ASSERT_TRUE(status.ok());

  SwordFsChunk out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 0, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.index, 0);
  EXPECT_EQ(out.start_offset, 0);
  EXPECT_EQ(out.key, "key");
  EXPECT_EQ(out.size, 100);
}

TEST_F(MemMetaStoreTest, AddChunkDuplicateFails) {
  SwordFsChunk chunk = MakeChunk(0, 0, 100);
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.AddChunk(42, chunk); });
  ASSERT_TRUE(status.ok());
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.AddChunk(42, chunk); });
  EXPECT_TRUE(status.IsAlreadyExists());
}

TEST_F(MemMetaStoreTest, FindChunkNotFound) {
  SwordFsChunk out;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 0, &out); });
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// TruncateChunks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, TruncateChunksNoChunksIsNoOp) {
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 0); });
  EXPECT_TRUE(status.ok());
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 100); });
  EXPECT_TRUE(status.ok());
}

TEST_F(MemMetaStoreTest, TruncateChunksToZeroRemovesAllChunks) {
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.AddChunk(42, MakeChunk(0, 0, 100));
    if (!status.ok()) {
      return status;
    }
    return txn.AddChunk(42, MakeChunk(1, 100, 100));
  });
  ASSERT_TRUE(status.ok());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 0); });
  ASSERT_TRUE(status.ok());
  SwordFsChunk out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 0, &out); });
  EXPECT_TRUE(status.IsNotFound());
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 1, &out); });
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, TruncateChunksDropsChunksBeyondNewSize) {
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.AddChunk(42, MakeChunk(0, 0, 100));
    if (!status.ok()) {
      return status;
    }
    return txn.AddChunk(42, MakeChunk(1, 100, 100));
  });
  ASSERT_TRUE(status.ok());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 100); });
  ASSERT_TRUE(status.ok());
  SwordFsChunk out;
  // Chunk 0 ends exactly at the new size — kept unchanged.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 0, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size, 100);
  // Chunk 1 starts at the new size — dropped.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 1, &out); });
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, TruncateChunksClampsStraddlingChunk) {
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.AddChunk(42, MakeChunk(0, 0, 100));
    if (!status.ok()) {
      return status;
    }
    return txn.AddChunk(42, MakeChunk(1, 100, 100));
  });
  ASSERT_TRUE(status.ok());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 150); });
  ASSERT_TRUE(status.ok());
  SwordFsChunk out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 0, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size, 100);
  // Chunk 1 straddles the new size (covers 100..200) — clamped to 50.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 1, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size, 50);
}

// ────────────────────────────────────────────────────────────────
// ReclaimInode (open-unlink)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ReclaimInodeMissingInodeIsNoOp) {
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ReclaimInode(999); });
  EXPECT_TRUE(status.ok());
}

TEST_F(MemMetaStoreTest, ReclaimInodeDeletesOrphanedInode) {
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;

  auto count = [&] { return store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); }); };

  // Unlink detaches the entry and drops nlink to 0; the inode survives.
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "f"); });
  ASSERT_TRUE(status.ok());

  EXPECT_EQ(count(), 2);
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.ReclaimInode(ino); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(count(), 1);
  EXPECT_TRUE(Lookup(ino).IsNotFound());
}

TEST_F(MemMetaStoreTest, ReclaimInodeKeepsLinkedInode) {
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;  // nlink == 1 — not orphaned

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ReclaimInode(ino); });
  ASSERT_TRUE(status.ok());
  size_t count = store_->Transact([&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(count, 2);
  SwordFsInode out;
  ASSERT_TRUE(Lookup(ino, &out).ok());
  EXPECT_EQ(out.ino, f.ino);
}

// ────────────────────────────────────────────────────────────────
// ListChunks — drives the VFS coordinator's chunk enumeration path.
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ListChunksEmptyInodeIsOk) {
  // No chunks registered: ListChunks must return an empty vector,
  // not an error. The coordinator relies on this to distinguish
  // "no data to delete" from "metadata failure".
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);

  std::vector<SwordFsChunk> out;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListChunks(f.ino, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_TRUE(out.empty());
}

TEST_F(MemMetaStoreTest, ListChunksNullOutIsError) {
  // Defensive: callers must hand in a valid pointer. A null out
  // would silently lose the chunks the coordinator needs to drive
  // data-engine Deletes — better to fail loudly.
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListChunks(1, nullptr); });
  EXPECT_TRUE(status.code() == Status::kInvalidArgument);
}

TEST_F(MemMetaStoreTest, ListChunksReturnsRegisteredChunksInIndexOrder) {
  constexpr uint64_t kChunkSize = 65536;
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;

  // Add three chunks out of order; the snapshot must be sorted by
  // ChunkIndex so the coordinator's Delete calls follow the same
  // order as a sequential reader would.
  SwordFsChunk c2{};
  c2.index = 2;
  c2.start_offset = 2 * kChunkSize;
  c2.key = std::to_string(ino) + "/2";
  c2.size = 100;
  SwordFsChunk c0{};
  c0.index = 0;
  c0.start_offset = 0;
  c0.key = std::to_string(ino) + "/0";
  c0.size = 100;
  SwordFsChunk c1{};
  c1.index = 1;
  c1.start_offset = kChunkSize;
  c1.key = std::to_string(ino) + "/1";
  c1.size = 100;
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.AddChunk(ino, c2);
    if (!status.ok()) {
      return status;
    }
    status = txn.AddChunk(ino, c0);
    if (!status.ok()) {
      return status;
    }
    return txn.AddChunk(ino, c1);
  });
  ASSERT_TRUE(status.ok());

  std::vector<SwordFsChunk> out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListChunks(ino, &out); });
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(out.size(), 3);
  EXPECT_EQ(out[0].index, 0);
  EXPECT_EQ(out[1].index, 1);
  EXPECT_EQ(out[2].index, 2);
}
