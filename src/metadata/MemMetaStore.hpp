// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed metadata store. Everything is held in RAM and lost on unmount.

#pragma once

#define FUSE_USE_VERSION 312
#include <folly/container/F14Map.h>
#include <fuse_lowlevel.h>

#include <mutex>
#include <string>

#include "metadata/MetaStore.hpp"

namespace swordfs::metadata {

class MemMetaStore : public MetaStore {
 public:
  MemMetaStore();
  ~MemMetaStore() override;

  // Entry operations
  Status Lookup(InodeID parent, std::string_view name, InodeID* child_ino,
                struct stat* attr) override;
  Status GetAttr(InodeID ino, struct stat* attr) override;
  Status Create(InodeID parent, std::string_view name, mode_t mode,
                InodeID* child_ino, struct stat* attr) override;
  Status Unlink(InodeID parent, std::string_view name) override;
  Status Rename(InodeID old_parent, std::string_view old_name,
                InodeID new_parent, std::string_view new_name) override;
  Status SetAttr(InodeID ino, const struct stat* attr, int to_set,
                 struct stat* out_attr) override;
  Status Access(InodeID ino, int mask) override;
  Status Open(InodeID ino, uint64_t* fh) override;
  Status Release(uint64_t fh) override;

  // Directory operations
  Status ReadDir(InodeID ino, std::vector<SwordFsEntry>* entries) override;
  Status MkDir(InodeID parent, std::string_view name, mode_t mode,
               InodeID* child_ino, struct stat* attr) override;
  Status RmDir(InodeID parent, std::string_view name) override;
  Status OpenDir(InodeID ino, uint64_t* fh) override;
  Status ReleaseDir(uint64_t fh) override;
  Status Forget(InodeID ino, uint64_t nlookup) override;

  // Volume operations
  Status StatFs(struct statvfs* stbuf) override;

 private:
  InodeID AllocIno();
  uint64_t AllocFh();

  // Helpers
  Status LookupEntry(InodeID parent, std::string_view name, SwordFsInode* child);
  void TouchParent(InodeID parent_ino);
  void TouchCtime(InodeID ino);
  void TouchAtime(InodeID ino);
  void KillSUID(struct stat* st);
  bool IsDir(InodeID ino) const;
  int Access(const struct stat* st, int mask) const;

 private:
  std::mutex mutex_;
  InodeID next_ino_{FUSE_ROOT_ID + 1};
  uint64_t next_fh_{1};

  InodeTable inodes_;
  // parent_ino -> (child_name -> child_ino)
  folly::F14FastMap<InodeID, EntryTable> dirs_;
  // File handle accounting: fh -> ino
  folly::F14FastMap<uint64_t, InodeID> file_handles_;
  // Directory handle accounting: fh -> ino
  folly::F14FastMap<uint64_t, InodeID> dir_handles_;
};

}  // namespace swordfs::metadata
