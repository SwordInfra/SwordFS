// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaStore.  Tests use the store exactly the way
// production code (MemMetaImpl) does: every operation — including
// single primitives — runs inside a Transact() script.  Read methods
// hand out snapshot COPIES of SwordFsInode, never pointers into
// store-owned memory.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>
#include <string>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/mem/MemMetaStore.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::MemMetaTxn;
using swordfs::metadata::ReclaimWork;
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
  Status List(InodeID ino, std::vector<SwordFsEntry> *entries) {
    return store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(ino, entries); });
  }

  MemMetaStore *store_;
};

// ────────────────────────────────────────────────────────────────
// Constructor
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, ConstructorCreatesRoot) {
  SwordFsInode root;
  EXPECT_TRUE(Lookup(kRoot, &root).ok());
  EXPECT_TRUE(root.IsDir());
  EXPECT_EQ(root.ino, kRoot);

  std::vector<SwordFsEntry> entries;
  ASSERT_TRUE(List(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 2U);
}

FIBER_TEST_F(MemMetaStoreTest, PrivateChunkIndexReadsStagedWritesAndDropsRejectedChanges) {
  ASSERT_TRUE(store_->Transact([](MemMetaTxn &txn) { return txn.Put("fragments", "first", "original"); }).ok());

  const auto rejected = store_->Transact([](MemMetaTxn &txn) {
    std::string value;
    EXPECT_TRUE(txn.Read("fragments", "first", &value).ok());
    EXPECT_EQ(value, "original");
    EXPECT_TRUE(txn.Put("fragments", "second", "new").ok());
    EXPECT_TRUE(txn.Read("fragments", "second", &value).ok());
    EXPECT_EQ(value, "new");
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_TRUE(txn.Scan("fragments", &values).ok());
    EXPECT_EQ(values.size(), 2U);
    EXPECT_TRUE(txn.Erase("fragments", "first").ok());
    EXPECT_TRUE(txn.Read("fragments", "first", &value).IsNotFound());
    EXPECT_TRUE(txn.Scan("fragments", &values).ok());
    EXPECT_EQ(values.size(), 1U);
    if (!values.empty()) {
      EXPECT_EQ(values[0].first, "second");
    }
    return Status::IOError("reject private index changes");
  });
  EXPECT_EQ(rejected.ToErrno(), EIO);

  ASSERT_TRUE(store_
                  ->Transact([](MemMetaTxn &txn) {
                    std::string value;
                    EXPECT_TRUE(txn.Read("fragments", "first", &value).ok());
                    EXPECT_EQ(value, "original");
                    EXPECT_TRUE(txn.Read("fragments", "second", &value).IsNotFound());
                    return txn.Erase("fragments", "first");
                  })
                  .ok());
  std::vector<std::pair<std::string, std::string>> values;
  EXPECT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.Scan("fragments", &values); }).ok());
  EXPECT_TRUE(values.empty());
}

FIBER_TEST_F(MemMetaStoreTest, PrivateChunkIndexRejectsInvalidHashAndOutputs) {
  EXPECT_TRUE(store_
                  ->Transact([](MemMetaTxn &txn) {
                    std::string value;
                    std::vector<std::pair<std::string, std::string>> values;
                    EXPECT_EQ(txn.Read("", "field", &value).ToErrno(), EINVAL);
                    EXPECT_EQ(txn.Read("hash", "field", nullptr).ToErrno(), EINVAL);
                    EXPECT_EQ(txn.Scan("", &values).ToErrno(), EINVAL);
                    EXPECT_EQ(txn.Scan("hash", nullptr).ToErrno(), EINVAL);
                    EXPECT_EQ(txn.Put("", "field", "value").ToErrno(), EINVAL);
                    EXPECT_EQ(txn.Erase("", "field").ToErrno(), EINVAL);
                    return Status::OK();
                  })
                  .ok());
}

// ────────────────────────────────────────────────────────────────
// LookupInode
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, LookupInodeNotFound) {
  SwordFsInode out;
  Status status = Lookup(999, &out);
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaStoreTest, LookupInodeWithNullOut) {
  EXPECT_TRUE(Lookup(kRoot).ok());
}

// ────────────────────────────────────────────────────────────────
// AddEntry
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, AddEntryCreatesFile) {
  SwordFsInode child;
  Status status = Add(kRoot, "hello.txt", kRegFile, &child);

  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(child.IsDir());
  EXPECT_GT(child.ino, 0);

  SwordFsInode stored;
  ASSERT_TRUE(Lookup(child.ino, &stored).ok());
  EXPECT_EQ(stored.ino, child.ino);

  std::vector<SwordFsEntry> entries;
  ASSERT_TRUE(List(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 3U);
}

FIBER_TEST_F(MemMetaStoreTest, AddEntryCreatesDirectory) {
  SwordFsInode child;
  Status status = Add(kRoot, "subdir", kDir, &child);

  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(child.IsDir());
}

FIBER_TEST_F(MemMetaStoreTest, AddEntryAlreadyExists) {
  Add(kRoot, "file", kRegFile);

  Status status = Add(kRoot, "file", kRegFile);
  EXPECT_TRUE(status.ToErrno() == EEXIST);
}

FIBER_TEST_F(MemMetaStoreTest, AddEntryParentNotFound) {
  Status status = Add(42, "orphan", kRegFile);
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaStoreTest, AddEntryParentNotDirectory) {
  // Create a regular file first
  SwordFsInode f;
  Add(kRoot, "regular", kRegFile, &f);

  // Try to add a child under the regular file
  Status status = Add(f.ino, "nested", kRegFile);
  EXPECT_TRUE(status.ToErrno() == ENOTDIR);
}

// ────────────────────────────────────────────────────────────────
// LookupEntry
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, LookupEntryFound) {
  SwordFsInode created;
  Add(kRoot, "found", kRegFile, &created);

  SwordFsInode out;
  Status status = LookupChild(kRoot, "found", &out);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(out.ino, created.ino);
}

FIBER_TEST_F(MemMetaStoreTest, LookupEntryNotFound) {
  Status status = LookupChild(kRoot, "nonexistent");
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// MoveEntry
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, MoveEntrySuccess) {
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

FIBER_TEST_F(MemMetaStoreTest, MoveEntryOldParentNotFound) {
  SwordFsInode sub;
  Add(kRoot, "dst", kDir, &sub);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(999, "f", sub.ino, "f", false); });
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaStoreTest, MoveEntryNewParentNotFound) {
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(kRoot, "f", 999, "f", false); });
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaStoreTest, MoveEntryTargetExists) {
  SwordFsInode d1;
  Add(kRoot, "d1", kDir, &d1);

  SwordFsInode d2;
  Add(kRoot, "d2", kDir, &d2);

  // Both have a "f" entry
  Add(d1.ino, "f", kRegFile);
  Add(d2.ino, "f", kRegFile);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(d1.ino, "f", d2.ino, "f", false); });
  EXPECT_TRUE(status.ToErrno() == EEXIST);
}

FIBER_TEST_F(MemMetaStoreTest, MoveEntryOverwriteWorksWithoutResultOutput) {
  SwordFsInode src_dir;
  SwordFsInode dst_dir;
  ASSERT_TRUE(Add(kRoot, "src", kDir, &src_dir).ok());
  ASSERT_TRUE(Add(kRoot, "dst", kDir, &dst_dir).ok());

  SwordFsInode source;
  SwordFsInode victim;
  ASSERT_TRUE(Add(src_dir.ino, "f", kRegFile, &source).ok());
  ASSERT_TRUE(Add(dst_dir.ino, "f", kRegFile, &victim).ok());

  const auto status =
      store_->Transact([&](MemMetaTxn &txn) { return txn.MoveEntry(src_dir.ino, "f", dst_dir.ino, "f", true); });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode moved;
  ASSERT_TRUE(LookupChild(dst_dir.ino, "f", &moved).ok());
  EXPECT_EQ(moved.ino, source.ino);
  EXPECT_TRUE(LookupChild(src_dir.ino, "f").IsNotFound());

  SwordFsInode orphaned_victim;
  ASSERT_TRUE(Lookup(victim.ino, &orphaned_victim).ok());
  EXPECT_EQ(orphaned_victim.attr.nlink, 0U);
}

// ────────────────────────────────────────────────────────────────
// Unlink
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, UnlinkOnlyRemovesDirectoryEntry) {
  // Unlink only detaches the directory entry and decrements nlink.
  // The inode survives until the background reclaimer crosses the metadata
  // point of no return. Foreground unlink and last-close no longer execute GC.
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "f"); });
  EXPECT_TRUE(status.ok());

  // Entry gone from directory
  EXPECT_TRUE(LookupChild(kRoot, "f").IsNotFound());
  // Inode and its metadata stay alive (nlink == 0).
  EXPECT_TRUE(Lookup(ino).ok());

  // Caller follows up with the reclaim protocol: preparation freezes the
  // (empty) work and drops the live inode, completion removes the record.
  std::optional<ReclaimWork> work;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(ino, &work); });
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(work.has_value());
  EXPECT_EQ(work->ino, ino);
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, 0, &refs).ok());
  EXPECT_TRUE(refs.empty());
  EXPECT_TRUE(Lookup(ino).IsNotFound());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.CompleteReclaim(ino); });
  EXPECT_TRUE(status.ok());
}

FIBER_TEST_F(MemMetaStoreTest, UnlinkMissingEntryReturnsNotFound) {
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "nonexistent"); });
  EXPECT_TRUE(status.IsNotFound());
  EXPECT_TRUE(Lookup(kRoot).ok());
  std::vector<SwordFsEntry> entries;
  ASSERT_TRUE(List(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 2U);
}

FIBER_TEST_F(MemMetaStoreTest, UnlinkNonEmptyDirectory) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);

  // Add a file inside sub
  Add(sub.ino, "f", kRegFile);

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "sub"); });
  EXPECT_TRUE(status.ToErrno() == ENOTEMPTY);
  EXPECT_TRUE(LookupChild(kRoot, "sub").ok());
  EXPECT_TRUE(LookupChild(sub.ino, "f").ok());
}

FIBER_TEST_F(MemMetaStoreTest, UnlinkEmptyDirectory) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);
  InodeID sub_ino = sub.ino;

  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "sub"); });
  EXPECT_TRUE(status.ok());
  // Both sub and its dir entry table freed
  EXPECT_TRUE(Lookup(sub_ino).IsNotFound());
  std::vector<SwordFsEntry> entries;
  ASSERT_TRUE(List(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 2U);
}

// ────────────────────────────────────────────────────────────────
// ListEntries
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, ListEntriesSuccess) {
  Add(kRoot, "a.txt", kRegFile);
  Add(kRoot, "b.txt", kRegFile);
  Add(kRoot, "c", kDir);

  std::vector<SwordFsEntry> entries;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(kRoot, &entries); });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(entries.size(), 5);  // 3 real + "." + ".."
}

FIBER_TEST_F(MemMetaStoreTest, ListEntriesEmptyDir) {
  std::vector<SwordFsEntry> entries;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(kRoot, &entries); });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(entries.size(), 2);  // just "." and ".."
}

FIBER_TEST_F(MemMetaStoreTest, ListEntriesNotFound) {
  std::vector<SwordFsEntry> entries;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.ListEntries(42, &entries); });
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// IsDescendantOf
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, IsDescendantOfDirectChild) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);

  bool result = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(kRoot, sub.ino); });
  EXPECT_TRUE(result);
}

FIBER_TEST_F(MemMetaStoreTest, IsDescendantOfGrandchild) {
  SwordFsInode sub;
  Add(kRoot, "sub", kDir, &sub);
  SwordFsInode leaf;
  Add(sub.ino, "nested", kRegFile, &leaf);

  bool from_root = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(kRoot, leaf.ino); });
  bool from_sub = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(sub.ino, leaf.ino); });
  EXPECT_TRUE(from_root);
  EXPECT_TRUE(from_sub);
}

FIBER_TEST_F(MemMetaStoreTest, IsDescendantOfNotDescendant) {
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

FIBER_TEST_F(MemMetaStoreTest, IsDescendantOfSelf) {
  bool result = store_->Transact([&](MemMetaTxn &txn) { return txn.IsDescendantOf(kRoot, kRoot); });
  EXPECT_FALSE(result);
}

// ────────────────────────────────────────────────────────────────
// Chunk metadata: CommitChunk / FindChunk
// ────────────────────────────────────────────────────────────────

namespace {
SwordFsChunk MakeChunk(ChunkIndex index, uint64_t start_offset, size_t size) {
  SwordFsChunk chunk;
  chunk.index = index;
  chunk.start_offset = start_offset;
  chunk.revision = static_cast<uint64_t>(index) + 1;
  chunk.size = size;
  return chunk;
}
}  // namespace

FIBER_TEST_F(MemMetaStoreTest, CommitChunkAndFindChunk) {
  SwordFsInode file;
  Add(kRoot, "chunk-file", kRegFile, &file);

  Status status =
      store_->Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(file.ino, std::nullopt, MakeChunk(0, 0, 100)); });
  ASSERT_TRUE(status.ok());

  SwordFsChunk out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 0, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.index, 0);
  EXPECT_EQ(out.start_offset, 0);
  EXPECT_EQ(out.revision, 1U);
  EXPECT_EQ(out.size, 100);
}

FIBER_TEST_F(MemMetaStoreTest, CommitChunkConflictingInitialPublicationFails) {
  SwordFsInode file;
  Add(kRoot, "conflicting-chunk-file", kRegFile, &file);

  SwordFsChunk chunk = MakeChunk(0, 0, 100);
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(file.ino, std::nullopt, chunk); });
  ASSERT_TRUE(status.ok());
  chunk.revision++;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(file.ino, std::nullopt, chunk); });
  EXPECT_TRUE(status.ToErrno() == EEXIST);
}

FIBER_TEST_F(MemMetaStoreTest, FindChunkNotFound) {
  SwordFsChunk out;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(42, 0, &out); });
  EXPECT_TRUE(status.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// TruncateChunks
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, TruncateChunksNoChunksIsNoOp) {
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 0); });
  EXPECT_TRUE(status.ok());
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(42, 100); });
  EXPECT_TRUE(status.ok());
}

FIBER_TEST_F(MemMetaStoreTest, TruncateChunksToZeroRemovesAllChunks) {
  SwordFsInode file;
  Add(kRoot, "truncate-zero", kRegFile, &file);
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.CommitChunk(file.ino, std::nullopt, MakeChunk(0, 0, 100));
    if (!status.ok()) {
      return status;
    }
    return txn.CommitChunk(file.ino, std::nullopt, MakeChunk(1, 100, 100));
  });
  ASSERT_TRUE(status.ok());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(file.ino, 0); });
  ASSERT_TRUE(status.ok());
  SwordFsChunk out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 0, &out); });
  EXPECT_TRUE(status.IsNotFound());
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 1, &out); });
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaStoreTest, TruncateChunksDropsChunksBeyondNewSize) {
  SwordFsInode file;
  Add(kRoot, "truncate-drop", kRegFile, &file);
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.CommitChunk(file.ino, std::nullopt, MakeChunk(0, 0, 100));
    if (!status.ok()) {
      return status;
    }
    return txn.CommitChunk(file.ino, std::nullopt, MakeChunk(1, 100, 100));
  });
  ASSERT_TRUE(status.ok());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(file.ino, 100); });
  ASSERT_TRUE(status.ok());
  SwordFsChunk out;
  // Chunk 0 ends exactly at the new size — kept unchanged.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 0, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size, 100);
  // Chunk 1 starts at the new size — dropped.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 1, &out); });
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaStoreTest, TruncateChunksClampsStraddlingChunk) {
  SwordFsInode file;
  Add(kRoot, "truncate-clamp", kRegFile, &file);
  Status status = store_->Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.CommitChunk(file.ino, std::nullopt, MakeChunk(0, 0, 100));
    if (!status.ok()) {
      return status;
    }
    return txn.CommitChunk(file.ino, std::nullopt, MakeChunk(1, 100, 100));
  });
  ASSERT_TRUE(status.ok());

  status = store_->Transact([&](MemMetaTxn &txn) { return txn.TruncateChunks(file.ino, 150); });
  ASSERT_TRUE(status.ok());
  SwordFsChunk out;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 0, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size, 100);
  // Chunk 1 straddles the new size (covers 100..200) — clamped to 50.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file.ino, 1, &out); });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size, 50);
}

// ────────────────────────────────────────────────────────────────
// Reclaim (open-unlink)
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaStoreTest, PrepareReclaimMissingInodeIsNoOp) {
  std::optional<ReclaimWork> work;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(999, &work); });
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(work.has_value());

  // Completing a reclaim that was never prepared is a no-op, not an error.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.CompleteReclaim(999); });
  EXPECT_TRUE(status.ok());
}

FIBER_TEST_F(MemMetaStoreTest, ReclaimPrimitivesValidateOutputsAndRejectDirectories) {
  EXPECT_EQ(store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(kRoot, nullptr); }).ToErrno(), EINVAL);
  EXPECT_EQ(store_->Transact([&](MemMetaTxn &txn) { return txn.ListOrphanCandidates(nullptr); }).ToErrno(), EINVAL);
  EXPECT_EQ(store_->Transact([&](MemMetaTxn &txn) { return txn.ListPendingReclaims(nullptr); }).ToErrno(), EINVAL);

  // Directories are never candidates for this file-data reclaim lifecycle.
  // Exercise that branch independently from the regular-file nlink>0 case.
  std::optional<ReclaimWork> work;
  const auto status = store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(kRoot, &work); });
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(work.has_value());
}

FIBER_TEST_F(MemMetaStoreTest, PrepareReclaimFreezesWorkAndDropsOrphanedInode) {
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;

  Status status =
      store_->Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(ino, std::nullopt, MakeChunk(0, 0, 100)); });
  ASSERT_TRUE(status.ok());

  // Unlink detaches the entry, drops nlink to 0 and publishes the inode as an
  // orphan candidate; the inode (and its chunk metadata) survives for now.
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRoot, "f"); });
  ASSERT_TRUE(status.ok());
  EXPECT_TRUE(Lookup(ino).ok());

  std::vector<InodeID> candidates;
  ASSERT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.ListOrphanCandidates(&candidates); }).ok());
  EXPECT_EQ(candidates, std::vector<InodeID>{ino});

  // Preparation freezes the authoritative object identity and drops the live
  // inode in one transaction.
  std::optional<ReclaimWork> work;
  status = store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(ino, &work); });
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(work.has_value());
  EXPECT_EQ(work->ino, ino);
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, 0, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, MakeChunk(0, 0, 100));
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(ino, 0, 1));
  EXPECT_TRUE(Lookup(ino).IsNotFound());

  std::vector<ReclaimWork> pending;
  ASSERT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.ListPendingReclaims(&pending); }).ok());
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending[0], *work);
  // The orphan marker is gone too: the candidate list and the frozen record
  // are never both live for the same inode.
  ASSERT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.ListOrphanCandidates(&candidates); }).ok());
  EXPECT_TRUE(candidates.empty());

  // Replaying preparation returns the same frozen work without changing it.
  std::optional<ReclaimWork> replay;
  ASSERT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(ino, &replay); }).ok());
  EXPECT_EQ(replay, work);

  // Completion drops the frozen record; it is idempotent.
  ASSERT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.CompleteReclaim(ino); }).ok());
  ASSERT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.ListPendingReclaims(&pending); }).ok());
  EXPECT_TRUE(pending.empty());
  EXPECT_TRUE(store_->Transact([&](MemMetaTxn &txn) { return txn.CompleteReclaim(ino); }).ok());
}

FIBER_TEST_F(MemMetaStoreTest, PrepareReclaimKeepsLinkedInode) {
  SwordFsInode f;
  Add(kRoot, "f", kRegFile, &f);
  InodeID ino = f.ino;  // nlink == 1 — not orphaned

  std::optional<ReclaimWork> work;
  Status status = store_->Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(ino, &work); });
  EXPECT_TRUE(status.ok());
  EXPECT_FALSE(work.has_value());
  SwordFsInode out;
  ASSERT_TRUE(Lookup(ino, &out).ok());
  EXPECT_EQ(out.ino, f.ino);
}

// ────────────────────────────────────────────────────────────────
// ListChunks — drives the VFS coordinator's chunk enumeration path.
