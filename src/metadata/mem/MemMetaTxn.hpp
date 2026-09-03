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
// The transaction API is deliberately semantic: callers should not need
// to reconstruct multi-field metadata transitions from low-level writes.
//
// Primitives maintain the tree's STRUCTURAL INVARIANTS themselves:
//   - creating/removing a subdirectory entry adjusts the parent's
//     nlink (the child's ".." backlink);
//   - moving an entry across parents adjusts both parents' nlink;
//   - any entry-list change bumps the parent directories' mtime/ctime;
//   - re-linking an inode (move/swap/link) bumps its ctime.
// Callers compose primitives for POLICY (permissions, POSIX error
// codes, flag dispatch) and never repeat this bookkeeping.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
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
  uint64_t InodeCount();

  // ────────────────────────────────────────────────────────────────
  // Inode writes (by ino)
  // ────────────────────────────────────────────────────────────────

  // Apply the requested SetAttr fields atomically. Size changes also
  // update chunk metadata and apply the killpriv/ctime rules.
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out = nullptr);

  // Truncate an inode atomically: update st_size, apply killpriv/ctime,
  // and drop or clamp chunk metadata beyond the new size.
  Status Truncate(InodeID ino, uint64_t size);

  // Bump the inode's atime/mtime/ctime to now, selected by |fields|
  // (only kAtime/kMtime/kCtime are honoured).
  Status TouchInode(InodeID ino, SetAttrField fields);

  // Add |delta| (may be negative) to the inode's nlink.
  Status AdjustNlink(InodeID ino, int delta);

  // Set the symlink target of |ino| and update st_size to match.
  Status SetSymlinkTarget(InodeID ino, std::string_view target);

  // ────────────────────────────────────────────────────────────────
  // Directory operations
  // ────────────────────────────────────────────────────────────────

  // Look up a child entry by name.  On success, *out receives a
  // snapshot copy of the child's inode record.
  Status LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out);

  // Allocate a new inode and link it as a child of parent.  Returns
  // AlreadyExists if the name already exists under that parent.  On
  // success, *out receives a snapshot copy of the new inode.
  // Linking a subdirectory also increments the parent's nlink (the new
  // ".." backlink); any successful link bumps the parent's mtime/ctime.
  Status AddEntry(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out);

  // Move an existing entry from old_parent/old_name to
  // new_parent/new_name.  The inode is re-linked, not re-created.
  //
  // The primitive enforces the tree's structural integrity itself:
  // a directory can never be moved into itself or its own subtree
  // (InvalidArgument), and moving onto itself — the same inode,
  // possibly through another hard link — is a no-op.
  //
  // When |overwrite| is false an existing target yields AlreadyExists.
  // When true, the target is atomically replaced: a directory target
  // must be empty (NotEmpty) and is reclaimed by the unlink; a file
  // target is detached and reported through |result| when its nlink
  // drops to zero so the VFS layer can perform open-fd-aware cleanup.
  // A directory/non-directory mismatch yields IsDirectory / NotDirectory.
  //
  // Moving a directory across parents adjusts both parents' nlink;
  // bumps both parents' mtime/ctime and the moved inode's ctime.
  Status MoveEntry(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino, std::string_view new_name,
                   bool overwrite, RenameResult *result = nullptr);

  // POSIX unlink(2): remove the child entry from its parent and
  // decrement nlink.  Does NOT delete file inodes or their data; that
  // is the caller's responsibility.  Empty-directory targets are
  // reclaimed immediately (and the parent loses the ".." backlink);
  // a non-empty directory returns NotEmpty.  Missing entries return
  // NotFound. Any successful removal bumps the parent's mtime/ctime.
  Status Unlink(InodeID parent_ino, std::string_view name, UnlinkResult *result = nullptr);

  // Link an existing inode (by ino) into a directory (hard link).
  // Increments the inode's nlink; bumps the inode's ctime and the
  // parent's mtime/ctime.
  Status LinkExistingEntry(InodeID parent_ino, std::string_view name, InodeID ino, SwordFsInode *out = nullptr);

  // List all entries in a directory, including the synthetic "."
  // and "..".
  Status ListEntries(InodeID ino, std::vector<SwordFsEntry> *entries);

  // Return true if child is a descendant of ancestor.
  bool IsDescendantOf(InodeID ancestor_ino, InodeID child_ino) const;

  // Swap two directory entries.  Keeps both inodes' parent_ino in sync
  // with their new locations; bumps both inodes' ctime and both
  // parents' mtime/ctime.  Needs no nlink adjustment: each parent
  // loses one entry and gains one.
  //
  // Structural integrity is enforced here: neither directory may end
  // up inside its own subtree (InvalidArgument).
  Status SwapEntries(InodeID parent_a_ino, std::string_view name_a, InodeID parent_b_ino, std::string_view name_b);

  // ────────────────────────────────────────────────────────────────
  // Chunk metadata
  // ────────────────────────────────────────────────────────────────

  Status AddChunk(InodeID ino, const SwordFsChunk &chunk);
  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);
  Status TruncateChunks(InodeID ino, uint64_t new_size);

  // ────────────────────────────────────────────────────────────────
  // Open-unlink reclaim
  // ────────────────────────────────────────────────────────────────

  Status StartSession();
  Status RefreshSession();
  Status StopSession();
  Status AcquireOpen(InodeID ino);
  Status ReleaseOpen(InodeID ino, bool *reclaimable);
  Status ReapStaleSessions();

  // Move an orphaned inode out of the live inode table while preserving its
  // chunk metadata as a pending data-reclaim job. No-op for linked inodes.
  Status ReclaimInode(InodeID ino);

  Status ListOrphanedInodes(std::vector<InodeID> *out);
  Status ListPendingReclaims(std::vector<InodeID> *out);
  Status ListReclaimChunks(InodeID ino, std::vector<SwordFsChunk> *out);
  Status CompleteReclaim(InodeID ino);

  // Snapshot every chunk registered for live |ino|, ascending chunk index.
  Status ListChunks(InodeID ino, std::vector<SwordFsChunk> *out);

 private:
  friend class MemMetaStore;

  // Only MemMetaStore::Transact() may begin a transaction.
  explicit MemMetaTxn(MemMetaStore *store) : store_(store) {
  }

  // ────────────────────────────────────────────────────────────────
  // Private helpers — direct accessors over the store's tables.  No
  // "Locked" suffix: every MemMetaTxn method runs inside the store's
  // critical section by construction.
  // ────────────────────────────────────────────────────────────────
  Status WriteAttr(InodeID ino, const SwordFsAttr &attr);

  SwordFsInode *FindInode(InodeID ino);
  void InsertInode(std::unique_ptr<SwordFsInode> inode);
  void DeleteInode(InodeID ino);
  SwordFsInode *FindEntry(InodeID parent_ino, std::string_view name);
  void LinkEntry(InodeID parent_ino, std::string_view name, SwordFsInode *inode);
  SwordFsInode *UnlinkEntry(InodeID parent_ino, std::string_view name);
  bool IsDirEmpty(InodeID ino);

  // Non-owning; the store outlives every transaction.
  MemMetaStore *store_;
};

}  // namespace swordfs::metadata
