// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Combined inode + directory manager for the memory backend.
//
// Transaction model:
//   All composition happens inside MemMetaStore::Transact().  The
//   callback receives a MemMetaTxn handle whose methods are the store's
//   primitive operations; the whole callback is one atomic step (for
//   the memory backend: a single critical section over mutex_).
//
//   The transaction interface uses VALUE SEMANTICS: reads hand out
//   snapshot copies of SwordFsInode and writes go through explicit
//   by-ino mutation primitives (TouchInode, AdjustNlink, WriteAttr,
//   ...).  No pointers into store-owned memory ever escape a
//   transaction, so a transaction script is a pure
//   read-snapshot -> compute -> write-back function.  This is the shape
//   a future KV/Redis backend maps onto a real transaction (optimistic
//   WATCH/MULTI/EXEC retry, or a Lua script).

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

class MemMetaStore;

// Handle for one atomic metadata transaction.  Obtained from
// MemMetaStore::Transact(); every method runs with the store's
// transaction context (for the memory backend: with mutex_ held).
// Never stored or used outside the Transact() callback.
//
// All reads return snapshot COPIES of the inode record; all writes go
// through by-ino mutation primitives.  Copies never alias store-owned
// memory, so they stay valid (and unchanged) for as long as the caller
// keeps them — but they may go stale with respect to later transactions.
class MemMetaTxn {
 public:
  // ────────────────────────────────────────────────────────────────
  // Inode reads (snapshot copies)
  // ────────────────────────────────────────────────────────────────

  // Look up an inode by number.  On success, *out receives a snapshot
  // copy of the inode record.
  Status LookupInode(InodeID ino, SwordFsInode *out);

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

  // Non-owning; the store outlives every transaction.
  MemMetaStore *store_;
};

class MemMetaStore {
 public:
  MemMetaStore();
  ~MemMetaStore();

  // Run |f| as one atomic transaction.  The callback receives a
  // MemMetaTxn whose methods are the store's primitive operations; the
  // whole callback executes as a single atomic step with respect to all
  // other transactions (memory backend: while holding mutex_).
  //
  // This is the seam where a future KV/Redis backend maps the same
  // callback shape onto a real transaction.
  template <typename F>
  decltype(auto) Transact(F&& f) {
    std::lock_guard<std::mutex> lock(mutex_);
    MemMetaTxn txn(this);
    return std::forward<F>(f)(txn);
  }

  // ────────────────────────────────────────────────────────────────
  // Single-operation convenience API
  // ────────────────────────────────────────────────────────────────
  // Each of these wraps exactly one primitive in its own transaction.
  // Callers composing multiple operations MUST use Transact() instead.
  // Read methods hand out snapshot copies — never pointers into
  // store-owned memory.

  // Look up an inode by number; *out receives a snapshot copy.
  Status LookupInode(InodeID ino, SwordFsInode *out);

  // Return the total number of inodes currently stored.
  size_t InodeCount();

  // Look up a child entry by name; *out receives a snapshot copy.
  Status LookupEntry(InodeID parent_ino, std::string_view name,
                     SwordFsInode *out);

  // Allocate a new inode and link it as a child of parent.  Returns
  // AlreadyExists if the name already exists under that parent.
  Status AddEntry(InodeID parent_ino, std::string_view name,
                  mode_t mode, uint64_t nlookup,
                  SwordFsInode *out);

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
  bool IsDescendantOf(InodeID ancestor_ino, InodeID child_ino);

  // Atomically swap two directory entries.
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
  // MemMetaTxn forwards to these helpers; its lifetime is exactly one
  // critical section over mutex_.
  friend class MemMetaTxn;

  // ────────────────────────────────────────────────────────────────
  // Private helpers — caller MUST hold mutex_ (i.e. run inside a
  // MemMetaTxn).
  // ────────────────────────────────────────────────────────────────
  SwordFsInode *FindInodeLocked(InodeID ino);
  void InsertInodeLocked(SwordFsInode *inode);
  void DeleteInodeLocked(InodeID ino);
  SwordFsInode *FindEntryLocked(InodeID parent_ino, std::string_view name);
  void LinkEntryLocked(InodeID parent_ino, std::string_view name,
                       SwordFsInode *inode);
  SwordFsInode *UnlinkEntryLocked(InodeID parent_ino, std::string_view name);
  bool IsDirEmptyLocked(InodeID ino);

 private:
  mutable std::mutex mutex_;
  std::atomic<InodeID> next_ino_;

  folly::F14FastMap<InodeID, SwordFsInode *> inodes_;
  folly::F14FastMap<InodeID, folly::F14FastMap<std::string, SwordFsInode *>> dirs_;

  // Chunk metadata: inode → (index → ChunkMeta).
  folly::F14FastMap<InodeID, folly::F14FastMap<ChunkIndex, ChunkMeta>> chunks_;
};

}  // namespace swordfs::metadata
