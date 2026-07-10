// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed metadata store. Everything is held in RAM and lost on unmount.

#pragma once

#include "metadata/MetaStore.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace swordfs::metadata {

class MemMetaStore : public MetaStore {
 public:
  MemMetaStore();

  Status Lookup(uint64_t parent, std::string_view name, uint64_t* child_ino,
                struct stat* attr) override;
  Status GetAttr(uint64_t ino, struct stat* attr) override;
  Status ReadDir(uint64_t ino, std::vector<DirEntry>* entries) override;
  Status Create(uint64_t parent, std::string_view name, mode_t mode,
                uint64_t* child_ino, struct stat* attr) override;
  Status MkDir(uint64_t parent, std::string_view name, mode_t mode,
               uint64_t* child_ino, struct stat* attr) override;
  Status Unlink(uint64_t parent, std::string_view name) override;
  Status RmDir(uint64_t parent, std::string_view name) override;
  Status Rename(uint64_t old_parent, std::string_view old_name,
                uint64_t new_parent, std::string_view new_name) override;
  Status SetAttr(uint64_t ino, const struct stat* attr, int to_set,
                 struct stat* out_attr) override;
  Status StatFs(struct statvfs* stbuf) override;
  Status Access(uint64_t ino, int mask) override;
  Status Open(uint64_t ino, uint64_t* fh) override;
  Status Release(uint64_t fh) override;
  Status OpenDir(uint64_t ino, uint64_t* fh) override;
  Status ReleaseDir(uint64_t fh) override;
  void Forget(uint64_t ino, uint64_t nlookup) override;

 private:
  uint64_t AllocIno();
  uint64_t AllocFh();

  // Helpers
  void TouchParent(uint64_t parent_ino);
  void TouchCtime(uint64_t ino);
  void TouchAtime(uint64_t ino);
  void KillSUID(struct stat* st);
  bool IsDir(uint64_t ino) const;
  int Access(const struct stat* st, int mask) const;

  struct InodeRecord {
    struct stat attr;
    uint64_t nlookup = 0;  // reserved for future forget support
  };

  std::mutex mutex_;
  uint64_t next_ino_{2};  // 1 is reserved for FUSE_ROOT_ID
  uint64_t next_fh_{1};

  std::unordered_map<uint64_t, InodeRecord> inodes_;
  // parent_ino -> (child_name -> child_ino)
  std::unordered_map<uint64_t, std::unordered_map<std::string, uint64_t>>
      dirs_;
  // File handle accounting: fh -> ino
  std::unordered_map<uint64_t, uint64_t> file_handles_;
  // Directory handle accounting: fh -> ino
  std::unordered_map<uint64_t, uint64_t> dir_handles_;
};

}  // namespace swordfs::metadata
