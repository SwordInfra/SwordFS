// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Combined inode + directory manager for the memory backend.
//
// Locking model:
//   Each public method acquires mutex_ once and holds it for its entire
//   duration.  Private helpers (suffixed _locked) assume the lock is
//   already held by the caller.

#pragma once

#include <folly/container/F14Map.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

using Status = swordfs::utils::Status;

namespace swordfs::metadata {

class MemMetaStore {
 public:
  MemMetaStore();
  ~MemMetaStore();

  // ────────────────────────────────────────────────────────────────
  // Inode operations
  // ────────────────────────────────────────────────────────────────

  // Look up an inode by number.  The returned pointer is valid only
  // until the next call to any MemMetaStore method.
  Status LookupInode(InodeID ino, SwordFsInode **out);

  // Return the total number of inodes currently stored.
  size_t InodeCount();

  // ────────────────────────────────────────────────────────────────
  // Directory operations
  // ────────────────────────────────────────────────────────────────

  // Look up a child entry by name. On success, *out receives the inode pointer.
  Status LookupEntry(InodeID parent_ino, std::string_view name,
                     SwordFsInode **out);

  // Allocate a new inode and link it as a child of parent. Returns
  // AlreadyExists if the name already exists under that parent.
  Status AddEntry(InodeID parent_ino, std::string_view name,
                  mode_t mode, uint64_t nlookup,
                  SwordFsInode **out);

  // Move an existing entry from old_parent/old_name to new_parent/new_name.
  // The inode is re-linked, not re-created.
  Status MoveEntry(InodeID old_parent_ino, std::string_view old_name,
                   InodeID new_parent_ino, std::string_view new_name);

  // POSIX unlink(2): remove the child entry from its parent and
  // decrement nlink. Does NOT delete the inode or its data; that is
  // the caller's responsibility (typically `VfsImpl::Unlink` will
  // follow up with `ReclaimData` once it has confirmed no open file
  // descriptor still references the inode).
  // A non-empty directory returns Busy; "." / ".." are rejected via the
  // permission layer above.
  Status Unlink(InodeID parent_ino, std::string_view name);

  // Link an existing inode into a directory (hard link). Increments nlink.
  Status LinkExistingEntry(InodeID parent_ino, std::string_view name,
                           SwordFsInode *inode);

  // List all (name, inode-pointer) pairs in a directory.
  Status ListEntries(InodeID ino,
                     std::vector<std::pair<std::string, SwordFsInode *>> *entries);

  // Return true if child is a descendant of ancestor.
  bool IsDescendantOf(InodeID ancestor_ino, InodeID child_ino) const;

  // Atomically swap two directory entries.  Acquires mutex_ for its
  // entire duration (public API convention).
  Status SwapEntries(InodeID parent_a_ino, std::string_view name_a,
                     InodeID parent_b_ino, std::string_view name_b);
  // ────────────────────────────────────────────────────────────────
  // Chunk metadata
  // ────────────────────────────────────────────────────────────────

  Status AddChunk(InodeID ino, const ChunkMeta &cm);
  Status FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta *cm);

  // Drop chunk metadata for data beyond |new_size|, truncating the
  // chunk that straddles the new size.  A |new_size| of 0 removes all
  // chunks.  Chunk byte ranges are derived from ChunkMeta::start_offset.
  Status TruncateChunks(InodeID ino, size_t new_size);

  // ────────────────────────────────────────────────────────────────
  // Open-unlink reclaim
  // ────────────────────────────────────────────────────────────────

  // Delete |ino| and its chunk-metadata map if the inode is orphaned
  // (nlink==0). No-op otherwise. The chunk objects themselves are NOT
  // deleted from the data engine here — the caller must enumerate the
  // chunk keys via `ListChunks` and call `IDataEngine::Delete` on each.
  Status ReclaimInode(InodeID ino);

  // Snapshot every chunk registered for |ino|. The order is ascending
  // chunk index so callers can issue data-engine Deletes in order and
  // log replay matches insertion order.
  Status ListChunks(InodeID ino, std::vector<ChunkMeta> *out);

 private:
  // ────────────────────────────────────────────────────────────────
  // Private helpers — caller MUST hold mutex_
  // ────────────────────────────────────────────────────────────────
  SwordFsInode *FindInodeLocked(InodeID ino);
  void InsertInodeLocked(SwordFsInode *inode);
  void DeleteInodeLocked(InodeID ino);
  SwordFsInode *FindEntryLocked(InodeID parent_ino, std::string_view name);
  void LinkEntryLocked(InodeID parent_ino, std::string_view name,
                       SwordFsInode *inode);
  SwordFsInode *UnlinkEntryLocked(InodeID parent_ino, std::string_view name);
  bool IsDirEmptyLocked(InodeID ino);

  bool IsDescendantOfImplLocked(InodeID current_ino, InodeID target_ino) const;

 private:
  mutable std::mutex mutex_;
  std::atomic<InodeID> next_ino_;

  folly::F14FastMap<InodeID, SwordFsInode *> inodes_;
  folly::F14FastMap<InodeID, folly::F14FastMap<std::string, SwordFsInode *>> dirs_;

  // Chunk metadata: inode → (index → ChunkMeta).
  folly::F14FastMap<InodeID, folly::F14FastMap<ChunkIndex, ChunkMeta>> chunks_;
};

}  // namespace swordfs::metadata
