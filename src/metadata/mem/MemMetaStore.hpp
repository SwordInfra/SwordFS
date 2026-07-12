// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Combined inode + directory manager for the memory backend.
// Thread-safe: all public methods acquire an internal mutex.

#pragma once

#include <folly/container/F14Map.h>

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

  // ── Inode operations ──

  Status LookupInode(InodeID ino, SwordFsInode** out);
  size_t InodeCount() const;

  // ── Directory operations ──

  // Look up a child entry by name. On success, *out is set to the stored
  // SwordFsInode pointer.
  Status LookupEntry(InodeID parent, std::string_view name, SwordFsInode** out);

  // Create a new inode and add it as a child entry. Allocates the inode
  // number internally. On success, *out is set to the new inode pointer.
  // Returns AlreadyExists if the name already exists.
  Status AddEntry(InodeID parent, std::string_view name,
                  mode_t mode, uint64_t nlookup,
                  SwordFsInode** out);

  // Move an existing inode from old_parent/old_name to new_parent/new_name.
  // The inode is re-linked, not destroyed.
  Status MoveEntry(InodeID old_parent, std::string_view old_name,
                   InodeID new_parent, std::string_view new_name);

  // Remove a child entry by name. If `destroy` is true, the child inode and
  // its directory table (if dir) are freed. A non-empty directory will be
  // refused with Busy.
  Status RemoveEntry(InodeID parent, std::string_view name, bool destroy = false);

  // List all (name, inode*) pairs in a directory.
  Status ListEntries(InodeID ino, std::vector<std::pair<std::string, SwordFsInode*>>* entries);

  // Return true if `child` is a descendant of `ancestor`.
  bool IsDescendantOf(InodeID ancestor, InodeID child) const;

 private:
  bool IsDescendantOfImpl(InodeID current, InodeID target) const;
  // Look up an entry by parent and name. Returns nullptr if not found.
  // Must be called with mutex_ held.
  SwordFsInode* FindEntry(InodeID parent, std::string_view name) const;

 private:
  mutable std::mutex mutex_;
  InodeID next_ino_;
  InodeTable inodes_;
  folly::F14FastMap<InodeID, EntryTable> dirs_;
};

}  // namespace swordfs::metadata
