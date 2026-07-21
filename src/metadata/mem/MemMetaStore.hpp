// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Combined inode + directory manager for the memory backend.

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

  // Look up an inode by number. On success, *out receives the pointer.
  Status LookupInode(InodeID ino, SwordFsInode** out);

  // Return the total number of inodes currently stored.
  size_t InodeCount();

  // ────────────────────────────────────────────────────────────────
  // Directory operations
  // ────────────────────────────────────────────────────────────────

  // Look up a child entry by name. On success, *out receives the inode pointer.
  Status LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode** out);

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

 private:
  // ────────────────────────────────────────────────────────────────
  // Inode-level helpers (each locks mutex_)
  // ────────────────────────────────────────────────────────────────
  SwordFsInode* FindInode(InodeID ino);
  void InsertInode(SwordFsInode* inode);
  void DeleteInode(InodeID ino);

  // ────────────────────────────────────────────────────────────────
  // Directory-entry helpers (each locks mutex_)
  // ────────────────────────────────────────────────────────────────
  SwordFsInode* FindEntry(InodeID parent_ino, std::string_view name);
  void LinkEntry(InodeID parent_ino, std::string_view name, SwordFsInode* inode);
  SwordFsInode* UnlinkEntry(InodeID parent_ino, std::string_view name);
  bool IsDirEmpty(InodeID ino);
  size_t ListDirEntries(InodeID ino, std::vector<std::pair<std::string, SwordFsInode*>>* out);

  // ────────────────────────────────────────────────────────────────
  // Tree traversal (locks mutex_)
  // ────────────────────────────────────────────────────────────────
  bool IsDescendantOfImpl(InodeID current_ino, InodeID target_ino) const;

  mutable std::mutex mutex_;
  std::atomic<InodeID> next_ino_;
  InodeTable inodes_;
  folly::F14FastMap<InodeID, EntryTable> dirs_;
};

}  // namespace swordfs::metadata
