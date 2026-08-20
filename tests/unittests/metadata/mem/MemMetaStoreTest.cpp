// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaStore.  The store interface uses value semantics:
// read methods (LookupInode/LookupEntry/AddEntry) hand out snapshot
// COPIES of SwordFsInode, never pointers into store-owned memory.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::ChunkMeta;
using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
static constexpr mode_t kRegFile = S_IFREG | 0644;
static constexpr mode_t kDir = S_IFDIR | 0755;

class MemMetaStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }

  MemMetaStore *store_;
};

// ────────────────────────────────────────────────────────────────
// Constructor & InodeCount
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ConstructorCreatesRoot) {
  EXPECT_EQ(store_->InodeCount(), 1);

  SwordFsInode root;
  EXPECT_TRUE(store_->LookupInode(kRoot, &root).ok());
  EXPECT_TRUE(root.IsDir());
  EXPECT_EQ(root.ino, kRoot);
}

// ────────────────────────────────────────────────────────────────
// LookupInode
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, LookupInodeNotFound) {
  SwordFsInode out;
  Status status = store_->LookupInode(999, &out);
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, LookupInodeWithNullOut) {
  EXPECT_TRUE(store_->LookupInode(kRoot, nullptr).ok());
}

// ────────────────────────────────────────────────────────────────
// AddEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, AddEntryCreatesFile) {
  SwordFsInode child;
  Status status = store_->AddEntry(kRoot, "hello.txt", kRegFile, 0, &child);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(child.IsDir());
  EXPECT_GT(child.ino, 0);
  EXPECT_EQ(store_->InodeCount(), 2);
}

TEST_F(MemMetaStoreTest, AddEntryCreatesDirectory) {
  SwordFsInode child;
  Status status = store_->AddEntry(kRoot, "subdir", kDir, 0, &child);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(child.IsDir());
}

TEST_F(MemMetaStoreTest, AddEntryAlreadyExists) {
  store_->AddEntry(kRoot, "file", kRegFile, 0, nullptr);

  Status status = store_->AddEntry(kRoot, "file", kRegFile, 0, nullptr);
  EXPECT_TRUE(status.IsAlreadyExists());
}

TEST_F(MemMetaStoreTest, AddEntryParentNotFound) {
  Status status = store_->AddEntry(42, "orphan", kRegFile, 0, nullptr);
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, AddEntryParentNotDirectory) {
  // Create a regular file first
  SwordFsInode f;
  store_->AddEntry(kRoot, "regular", kRegFile, 0, &f);

  // Try to add a child under the regular file
  Status status = store_->AddEntry(f.ino, "nested", kRegFile, 0, nullptr);
  EXPECT_TRUE(status.IsNotDirectory());
}

// ────────────────────────────────────────────────────────────────
// LookupEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, LookupEntryFound) {
  SwordFsInode created;
  store_->AddEntry(kRoot, "found", kRegFile, 0, &created);

  SwordFsInode out;
  Status status = store_->LookupEntry(kRoot, "found", &out);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(out.ino, created.ino);
}

TEST_F(MemMetaStoreTest, LookupEntryNotFound) {
  Status status = store_->LookupEntry(kRoot, "nonexistent", nullptr);
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// MoveEntry
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, MoveEntrySuccess) {
  // Create dir: root/sub/
  SwordFsInode sub;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);

  // Create file under root
  SwordFsInode f1;
  store_->AddEntry(kRoot, "f1", kRegFile, 0, &f1);

  // Move root/f1 → root/sub/f1
  Status status = store_->MoveEntry(kRoot, "f1", sub.ino, "f1");
  EXPECT_TRUE(status.ok());

  // Old location is gone
  EXPECT_TRUE(store_->LookupEntry(kRoot, "f1", nullptr).IsNotFound());

  // New location has it, with the same ino — re-linked, not copied
  SwordFsInode moved;
  EXPECT_TRUE(store_->LookupEntry(sub.ino, "f1", &moved).ok());
  EXPECT_EQ(moved.ino, f1.ino);
  // ... and its parent pointer followed the move
  EXPECT_EQ(moved.parent_ino, sub.ino);
}

TEST_F(MemMetaStoreTest, MoveEntryOldParentNotFound) {
  SwordFsInode sub;
  store_->AddEntry(kRoot, "dst", kDir, 0, &sub);

  Status status = store_->MoveEntry(999, "f", sub.ino, "f");
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, MoveEntryNewParentNotFound) {
  SwordFsInode f;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);

  Status status = store_->MoveEntry(kRoot, "f", 999, "f");
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaStoreTest, MoveEntryTargetExists) {
  SwordFsInode d1;
  store_->AddEntry(kRoot, "d1", kDir, 0, &d1);

  SwordFsInode d2;
  store_->AddEntry(kRoot, "d2", kDir, 0, &d2);

  // Both have a "f" entry
  store_->AddEntry(d1.ino, "f", kRegFile, 0, nullptr);
  store_->AddEntry(d2.ino, "f", kRegFile, 0, nullptr);

  Status status = store_->MoveEntry(d1.ino, "f", d2.ino, "f");
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
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  InodeID ino = f.ino;

  EXPECT_EQ(store_->InodeCount(), 2);  // root + f
  EXPECT_TRUE(store_->Unlink(kRoot, "f").ok());

  // Entry gone from directory
  EXPECT_TRUE(store_->LookupEntry(kRoot, "f", nullptr).IsNotFound());
  // Inode and its metadata stay alive (nlink == 0).
  EXPECT_EQ(store_->InodeCount(), 2);
  EXPECT_TRUE(store_->LookupInode(ino, nullptr).ok());

  // Caller follows up with ReclaimInode to free the orphaned inode.
  EXPECT_TRUE(store_->ReclaimInode(ino).ok());
  EXPECT_EQ(store_->InodeCount(), 1);
  EXPECT_TRUE(store_->LookupInode(ino, nullptr).IsNotFound());
}

TEST_F(MemMetaStoreTest, UnlinkIdempotent) {
  EXPECT_TRUE(store_->Unlink(kRoot, "nonexistent").ok());
  EXPECT_EQ(store_->InodeCount(), 1);
}

TEST_F(MemMetaStoreTest, UnlinkNonEmptyDirectory) {
  SwordFsInode sub;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);

  // Add a file inside sub
  store_->AddEntry(sub.ino, "f", kRegFile, 0, nullptr);

  Status status = store_->Unlink(kRoot, "sub");
  EXPECT_TRUE(status.IsNotEmpty());
  EXPECT_EQ(store_->InodeCount(), 3);  // root + sub + f
}

TEST_F(MemMetaStoreTest, UnlinkEmptyDirectory) {
  SwordFsInode sub;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);
  InodeID sub_ino = sub.ino;

  EXPECT_TRUE(store_->Unlink(kRoot, "sub").ok());
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

  std::vector<SwordFsEntry> entries;
  Status status = store_->ListEntries(kRoot, &entries);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(entries.size(), 5);  // 3 real + "." + ".."
}

TEST_F(MemMetaStoreTest, ListEntriesEmptyDir) {
  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(store_->ListEntries(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 2);  // just "." and ".."
}

TEST_F(MemMetaStoreTest, ListEntriesNotFound) {
  std::vector<SwordFsEntry> entries;
  Status status = store_->ListEntries(42, &entries);
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// IsDescendantOf
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, IsDescendantOfDirectChild) {
  SwordFsInode sub;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);

  EXPECT_TRUE(store_->IsDescendantOf(kRoot, sub.ino));
}

TEST_F(MemMetaStoreTest, IsDescendantOfGrandchild) {
  SwordFsInode sub;
  store_->AddEntry(kRoot, "sub", kDir, 0, &sub);
  SwordFsInode leaf;
  store_->AddEntry(sub.ino, "nested", kRegFile, 0, &leaf);

  EXPECT_TRUE(store_->IsDescendantOf(kRoot, leaf.ino));
  EXPECT_TRUE(store_->IsDescendantOf(sub.ino, leaf.ino));
}

TEST_F(MemMetaStoreTest, IsDescendantOfNotDescendant) {
  SwordFsInode a;
  store_->AddEntry(kRoot, "a", kDir, 0, &a);
  SwordFsInode b;
  store_->AddEntry(kRoot, "b", kDir, 0, &b);
  SwordFsInode leaf;
  store_->AddEntry(b.ino, "leaf", kRegFile, 0, &leaf);

  // leaf is under b, not under a
  EXPECT_FALSE(store_->IsDescendantOf(a.ino, leaf.ino));
}

TEST_F(MemMetaStoreTest, IsDescendantOfSelf) {
  EXPECT_FALSE(store_->IsDescendantOf(kRoot, kRoot));
}

// ────────────────────────────────────────────────────────────────
// Chunk metadata: AddChunk / FindChunk
// ────────────────────────────────────────────────────────────────

namespace {
ChunkMeta MakeChunk(ChunkIndex index, uint64_t start_offset, size_t size) {
  ChunkMeta cm;
  cm.index = index;
  cm.start_offset = start_offset;
  cm.key = "key";
  cm.size = size;
  return cm;
}
}  // namespace

TEST_F(MemMetaStoreTest, AddChunkAndFindChunk) {
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(0, 0, 100)).ok());

  ChunkMeta out;
  auto status = store_->FindChunk(42, 0, &out);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.index, 0);
  EXPECT_EQ(out.start_offset, 0);
  EXPECT_EQ(out.key, "key");
  EXPECT_EQ(out.size, 100);
}

TEST_F(MemMetaStoreTest, AddChunkDuplicateFails) {
  ChunkMeta cm = MakeChunk(0, 0, 100);
  ASSERT_TRUE(store_->AddChunk(42, cm).ok());
  EXPECT_TRUE(store_->AddChunk(42, cm).IsAlreadyExists());
}

TEST_F(MemMetaStoreTest, FindChunkNotFound) {
  ChunkMeta out;
  EXPECT_TRUE(store_->FindChunk(42, 0, &out).IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// TruncateChunks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, TruncateChunksNoChunksIsNoOp) {
  EXPECT_TRUE(store_->TruncateChunks(42, 0).ok());
  EXPECT_TRUE(store_->TruncateChunks(42, 100).ok());
}

TEST_F(MemMetaStoreTest, TruncateChunksToZeroRemovesAllChunks) {
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(0, 0, 100)).ok());
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(1, 100, 100)).ok());

  ASSERT_TRUE(store_->TruncateChunks(42, 0).ok());
  ChunkMeta out;
  EXPECT_TRUE(store_->FindChunk(42, 0, &out).IsNotFound());
  EXPECT_TRUE(store_->FindChunk(42, 1, &out).IsNotFound());
}

TEST_F(MemMetaStoreTest, TruncateChunksDropsChunksBeyondNewSize) {
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(0, 0, 100)).ok());
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(1, 100, 100)).ok());

  ASSERT_TRUE(store_->TruncateChunks(42, 100).ok());
  ChunkMeta out;
  // Chunk 0 ends exactly at the new size — kept unchanged.
  ASSERT_TRUE(store_->FindChunk(42, 0, &out).ok());
  EXPECT_EQ(out.size, 100);
  // Chunk 1 starts at the new size — dropped.
  EXPECT_TRUE(store_->FindChunk(42, 1, &out).IsNotFound());
}

TEST_F(MemMetaStoreTest, TruncateChunksClampsStraddlingChunk) {
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(0, 0, 100)).ok());
  ASSERT_TRUE(store_->AddChunk(42, MakeChunk(1, 100, 100)).ok());

  ASSERT_TRUE(store_->TruncateChunks(42, 150).ok());
  ChunkMeta out;
  ASSERT_TRUE(store_->FindChunk(42, 0, &out).ok());
  EXPECT_EQ(out.size, 100);
  // Chunk 1 straddles the new size (covers 100..200) — clamped to 50.
  ASSERT_TRUE(store_->FindChunk(42, 1, &out).ok());
  EXPECT_EQ(out.size, 50);
}

// ────────────────────────────────────────────────────────────────
// ReclaimInode (open-unlink)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreTest, ReclaimInodeMissingInodeIsNoOp) {
  EXPECT_TRUE(store_->ReclaimInode(999).ok());
}

TEST_F(MemMetaStoreTest, ReclaimInodeDeletesOrphanedInode) {
  SwordFsInode f;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  InodeID ino = f.ino;

  // Unlink detaches the entry and drops nlink to 0; the inode survives.
  ASSERT_TRUE(store_->Unlink(kRoot, "f").ok());

  EXPECT_EQ(store_->InodeCount(), 2);
  ASSERT_TRUE(store_->ReclaimInode(ino).ok());
  EXPECT_EQ(store_->InodeCount(), 1);
  EXPECT_TRUE(store_->LookupInode(ino, nullptr).IsNotFound());
}

TEST_F(MemMetaStoreTest, ReclaimInodeKeepsLinkedInode) {
  SwordFsInode f;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  InodeID ino = f.ino;  // nlink == 1 — not orphaned

  ASSERT_TRUE(store_->ReclaimInode(ino).ok());
  EXPECT_EQ(store_->InodeCount(), 2);
  SwordFsInode out;
  ASSERT_TRUE(store_->LookupInode(ino, &out).ok());
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
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);

  std::vector<ChunkMeta> out;
  ASSERT_TRUE(store_->ListChunks(f.ino, &out).ok());
  EXPECT_TRUE(out.empty());
}

TEST_F(MemMetaStoreTest, ListChunksNullOutIsError) {
  // Defensive: callers must hand in a valid pointer. A null out
  // would silently lose the chunks the coordinator needs to drive
  // data-engine Deletes — better to fail loudly.
  EXPECT_TRUE(store_->ListChunks(1, nullptr).code() == Status::kInvalidArgument);
}

TEST_F(MemMetaStoreTest, ListChunksReturnsRegisteredChunksInIndexOrder) {
  constexpr uint64_t kChunkSize = 65536;
  SwordFsInode f;
  store_->AddEntry(kRoot, "f", kRegFile, 0, &f);
  InodeID ino = f.ino;

  // Add three chunks out of order; the snapshot must be sorted by
  // ChunkIndex so the coordinator's Delete calls follow the same
  // order as a sequential reader would.
  ChunkMeta c2{};
  c2.index = 2;
  c2.start_offset = 2 * kChunkSize;
  c2.key = std::to_string(ino) + "/2";
  c2.size = 100;
  ChunkMeta c0{};
  c0.index = 0;
  c0.start_offset = 0;
  c0.key = std::to_string(ino) + "/0";
  c0.size = 100;
  ChunkMeta c1{};
  c1.index = 1;
  c1.start_offset = kChunkSize;
  c1.key = std::to_string(ino) + "/1";
  c1.size = 100;
  ASSERT_TRUE(store_->AddChunk(ino, c2).ok());
  ASSERT_TRUE(store_->AddChunk(ino, c0).ok());
  ASSERT_TRUE(store_->AddChunk(ino, c1).ok());

  std::vector<ChunkMeta> out;
  ASSERT_TRUE(store_->ListChunks(ino, &out).ok());
  ASSERT_EQ(out.size(), 3);
  EXPECT_EQ(out[0].index, 0);
  EXPECT_EQ(out[1].index, 1);
  EXPECT_EQ(out[2].index, 2);
}
