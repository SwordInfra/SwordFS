// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed metadata store. Everything is held in RAM and lost on unmount.

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "metadata/MetaStore.hpp"

namespace swordfs::metadata {

class MemMetaStore : public MetaStore {
 public:
  MemMetaStore();

  Status Lookup(InodeID parent, std::string_view name, InodeID* child_ino,
                struct stat* attr) override;
  Status GetAttr(InodeID ino, struct stat* attr) override;
  Status ReadDir(InodeID ino, std::vector<DirEntry>* entries) override;
  Status Create(InodeID parent, std::string_view name, mode_t mode,
                InodeID* child_ino, struct stat* attr) override;
  Status MkDir(InodeID parent, std::string_view name, mode_t mode,
               InodeID* child_ino, struct stat* attr) override;
  Status Unlink(InodeID parent, std::string_view name) override;
  Status RmDir(InodeID parent, std::string_view name) override;
  Status Rename(InodeID old_parent, std::string_view old_name,
                InodeID new_parent, std::string_view new_name) override;
  Status SetAttr(InodeID ino, const struct stat* attr, int to_set,
                 struct stat* out_attr) override;
  Status StatFs(struct statvfs* stbuf) override;
  Status Access(InodeID ino, int mask) override;
  Status Open(InodeID ino, uint64_t* fh) override;
  Status Release(uint64_t fh) override;
  Status OpenDir(InodeID ino, uint64_t* fh) override;
  Status ReleaseDir(uint64_t fh) override;
  Status Forget(InodeID ino, uint64_t nlookup) override;

 private:
  InodeID AllocIno();
  uint64_t AllocFh();

  // Helpers
  void TouchParent(InodeID parent_ino);
  void TouchCtime(InodeID ino);
  void TouchAtime(InodeID ino);
  void KillSUID(struct stat* st);
  bool IsDir(InodeID ino) const;
  int Access(const struct stat* st, int mask) const;

  struct InodeRecord {
    struct stat attr;
    uint64_t nlookup = 0;  // reserved for future forget support
  };

  std::mutex mutex_;
  InodeID next_ino_{2};  // 1 is reserved for FUSE_ROOT_ID
  uint64_t next_fh_{1};

  std::unordered_map<InodeID, InodeRecord> inodes_;
  // parent_ino -> (child_name -> child_ino)
  std::unordered_map<InodeID, std::unordered_map<std::string, InodeID>>
      dirs_;
  // File handle accounting: fh -> ino
  std::unordered_map<uint64_t, InodeID> file_handles_;
  // Directory handle accounting: fh -> ino
  std::unordered_map<uint64_t, InodeID> dir_handles_;
};

}  // namespace swordfs::metadata
