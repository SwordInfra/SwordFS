// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Handle for one atomic metadata transaction on the memory backend.
//
// Obtained from MemMetaStore::Transact(); every method runs with the
// store's transaction context (for the memory backend: with mutex_
// held).  Never stored or used outside the Transact() callback.
//
// All reads return snapshot COPIES of the inode record; all writes go
// through by-ino mutation primitives.  Copies never alias store-owned
// memory, so they stay valid (and unchanged) for as long as the caller
// keeps them — but they may go stale with respect to later transactions.
// This is the shape a future KV/Redis backend maps onto a real
// transaction (optimistic WATCH/MULTI/EXEC retry, or a Lua script).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

using Status = swordfs::utils::Status;

namespace swordfs::metadata {

class MemMetaStore;

class MemMetaTxn {
 public:
  // ────────────────────────────────────────────────────────────────
  // Inode reads (snapshot copies)
  // ────────────────────────────────────────────────────────────────

  // Look up an inode by number.  On success, *out receives a snapshot
  // copy of the inode record.
  Status LookupInode(InodeID ino, SwordFsInode *out);

  // Return the total number of inodes currently stored.
  size_t InodeCount();

  // ────────────────────────────────────────────────────────────────
  // Inode writes (by ino)
  // ────────────────────────────────────────────────────────────────

  // Overwrite the inode's full attribute record.  Callers are expected
  // to read-modify-write: take a snapshot via LookupInode, mutate the
  // copy, then write it back — all within the same transaction.
  Status WriteAttr(InodeID ino, const struct stat *attr);

  // Bump the inode's atime/mtime/ctime to now, selected by |fields|
  // (only kAtime/kMtime/kCtime are honoured).
  Status TouchInode(InodeID ino, SetAttrField fields);

  // Add |delta| (may be negative) to the inode's nlink.
  Status AdjustNlink(InodeID ino, int delta);

  // Add |delta| (may be negative) to the inode's lookup count.
  // The count saturates at zero: subtracting more than the current
  // value leaves it at 0.
  Status AddNlookup(InodeID ino, int64_t delta);

  // Set the symlink target of |ino| and update st_size to match.
  Status SetSymlinkTarget(InodeID ino, std::string_view target);

  // ────────────────────────────────────────────────────────────────
  // Directory operations
  // ────────────────────────────────────────────────────────────────

  // Look up a child entry by name.  On success, *out receives a
  // snapshot copy of the child's inode record.
  Status LookupEntry(InodeID parent_ino, std::string_view name,
                     SwordFsInode *out);

  // Allocate a new inode and link it as a child of parent.  Returns
  // AlreadyExists if the name already exists under that parent.  On
  // success, *out receives a snapshot copy of the new inode.
  Status AddEntry(InodeID parent_ino, std::string_view name,
                  mode_t mode, uint64_t nlookup, SwordFsInode *out);

  // Move an existing entry from old_parent/old_name to
  // new_parent/new_name.  The inode is re-linked, not re-created.
  Status MoveEntry(InodeID old_parent_ino, std::string_view old_name,
                   InodeID new_parent_ino, std::string_view new_name);

  // POSIX unlink(2): remove the child entry from its parent and
  // decrement nlink.  Does NOT delete file inodes or their data; that
  // is the caller's responsibility.  Empty-directory targets are
  // reclaimed immediately; a non-empty directory returns NotEmpty.
  Status Unlink(InodeID parent_ino, std::string_view name,
                nlink_t *post_nlink = nullptr);

  // Link an existing inode (by ino) into a directory (hard link).
  // Increments nlink.
  Status LinkExistingEntry(InodeID parent_ino, std::string_view name,
                           InodeID ino);

  // List all entries in a directory, including the synthetic "."
  // and "..".
  Status ListEntries(InodeID ino, std::vector<SwordFsEntry> *entries);

  // Return true if child is a descendant of ancestor.
  bool IsDescendantOf(InodeID ancestor_ino, InodeID child_ino) const;

  // Swap two directory entries.
  Status SwapEntries(InodeID parent_a_ino, std::string_view name_a,
                     InodeID parent_b_ino, std::string_view name_b);

  // ────────────────────────────────────────────────────────────────
  // Chunk metadata
  // ────────────────────────────────────────────────────────────────

  Status AddChunk(InodeID ino, const ChunkMeta &cm);
  Status FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta *cm);
  Status TruncateChunks(InodeID ino, size_t new_size);

  // ────────────────────────────────────────────────────────────────
  // Open-unlink reclaim
  // ────────────────────────────────────────────────────────────────

  // Delete |ino| and its chunk-metadata map if the inode is orphaned
  // (nlink==0).  No-op otherwise.
  Status ReclaimInode(InodeID ino);

  // Snapshot every chunk registered for |ino|, ascending chunk index.
  Status ListChunks(InodeID ino, std::vector<ChunkMeta> *out);

 private:
  friend class MemMetaStore;

  // Only MemMetaStore::Transact() may begin a transaction.
  explicit MemMetaTxn(MemMetaStore *store) : store_(store) {}

  // ────────────────────────────────────────────────────────────────
  // Private helpers — direct accessors over the store's tables.  No
  // "Locked" suffix: every MemMetaTxn method runs inside the store's
  // critical section by construction.
  // ────────────────────────────────────────────────────────────────
  SwordFsInode *FindInode(InodeID ino);
  void InsertInode(SwordFsInode *inode);
  void DeleteInode(InodeID ino);
  SwordFsInode *FindEntry(InodeID parent_ino, std::string_view name);
  void LinkEntry(InodeID parent_ino, std::string_view name,
                 SwordFsInode *inode);
  SwordFsInode *UnlinkEntry(InodeID parent_ino, std::string_view name);
  bool IsDirEmpty(InodeID ino);

  // Non-owning; the store outlives every transaction.
  MemMetaStore *store_;
};

}  // namespace swordfs::metadata
