// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed Meta implementation — thin facade around MemMetaStore
// that adds locking, file/dir handle accounting, and permission checks.

#pragma once

#include <mutex>
#include <string>

#include "metadata/Meta.hpp"
#include "metadata/mem/MemMetaStore.hpp"

namespace swordfs::metadata {

// ────────────────────────────────────────────────────────────────
// HandleTable — thread-safe handle-to-inode mapping.
// ────────────────────────────────────────────────────────────────

class HandleTable {
 public:
  // Allocate a new handle and map it to the given inode.
  uint64_t Alloc(InodeID ino);

  // Release a handle. Returns true if the handle was found and removed.
  bool Release(uint64_t fh);

 private:
  mutable std::mutex mutex_;
  uint64_t next_fh_{1};
  folly::F14FastMap<uint64_t, InodeID> handles_;
};

class MemMetaImpl : public Meta {
 public:
  MemMetaImpl();
  ~MemMetaImpl() override;

  // Entry operations
  Status Lookup(InodeID parent_ino,
                std::string_view name, InodeID* child_ino,
                struct stat* attr) override;
  Status GetAttr(InodeID ino, struct stat* attr) override;
  Status Create(InodeID parent_ino,
                std::string_view name, mode_t mode, InodeID* child_ino,
                struct stat* attr) override;
  Status Unlink(InodeID parent_ino, std::string_view name) override;
  Status Rename(InodeID old_parent_ino,
                std::string_view old_name, InodeID new_parent_ino,
                std::string_view new_name) override;
  Status SetAttr(InodeID ino,
                 const struct stat* attr, int to_set,
                 struct stat* out_attr) override;
  Status Access(InodeID ino, int mask) override;
  Status Open(InodeID ino, uint64_t* fh) override;
  Status Release(uint64_t fh) override;

  // Directory operations
  Status ReadDir(InodeID ino, std::vector<SwordFsEntry>* entries) override;
  Status MkDir(InodeID parent_ino,
               std::string_view name, mode_t mode, InodeID* child_ino,
               struct stat* attr) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status OpenDir(InodeID ino, uint64_t* fh) override;
  Status ReleaseDir(uint64_t fh) override;
  Status Forget(InodeID ino, uint64_t nlookup) override;

  // Volume operations
  Status StatFs(struct statvfs* stbuf) override;

 private:
  // Helpers
  void KillSUID(struct stat* st);

 private:
  MemMetaStore store_;
  HandleTable file_handles_;
  HandleTable dir_handles_;
};

}  // namespace swordfs::metadata
