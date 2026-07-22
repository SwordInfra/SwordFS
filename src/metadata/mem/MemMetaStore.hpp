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
  Status LookupInode(InodeID ino, SwordFsInode** out);

  // Return the total number of inodes currently stored.
  size_t InodeCount();

  // ────────────────────────────────────────────────────────────────
  // Directory operations
  // ────────────────────────────────────────────────────────────────

  // Look up a child entry by name. On success, *out receives the inode pointer.
  Status LookupEntry(InodeID parent_ino, std::string_view name,
                     SwordFsInode** out);

  // Allocate a new inode and link it as a child of parent. Returns
  // AlreadyExists if the name already exists under that parent.
  Status AddEntry(InodeID parent_ino, std::string_view name,
                  mode_t mode, uint64_t nlookup,
                  SwordFsInode** out);

  // Move an existing entry from old_parent/old_name to new_parent/new_name.
  // The inode is re-linked, not re-created.
  Status MoveEntry(InodeID old_parent_ino, std::string_view old_name,
                   InodeID new_parent_ino, std::string_view new_name);

  // Remove a child entry by name and free the inode (and directory table,
  // for directories). A non-empty directory returns Busy.
  Status RemoveEntry(InodeID parent_ino, std::string_view name);

  // List all (name, inode-pointer) pairs in a directory.
  Status ListEntries(InodeID ino,
                     std::vector<std::pair<std::string, SwordFsInode*>>* entries);

  // Return true if child is a descendant of ancestor.
  bool IsDescendantOf(InodeID ancestor_ino, InodeID child_ino) const;

  // Atomically swap two directory entries.  Acquires mutex_ for its
  // entire duration (public API convention).
  Status SwapEntries(InodeID parent_a_ino, std::string_view name_a,
                     InodeID parent_b_ino, std::string_view name_b);

 private:
  // ────────────────────────────────────────────────────────────────
  // Private helpers — caller MUST hold mutex_
  // ────────────────────────────────────────────────────────────────
  SwordFsInode* FindInodeLocked(InodeID ino);
  void InsertInodeLocked(SwordFsInode* inode);
  void DeleteInodeLocked(InodeID ino);
  SwordFsInode* FindEntryLocked(InodeID parent_ino, std::string_view name);
  void LinkEntryLocked(InodeID parent_ino, std::string_view name,
                       SwordFsInode* inode);
  SwordFsInode* UnlinkEntryLocked(InodeID parent_ino, std::string_view name);
  bool IsDirEmptyLocked(InodeID ino);

  bool IsDescendantOfImplLocked(InodeID current_ino, InodeID target_ino) const;

  mutable std::mutex mutex_;
  std::atomic<InodeID> next_ino_;
  InodeTable inodes_;
  folly::F14FastMap<InodeID, EntryTable> dirs_;
};

}  // namespace swordfs::metadata
