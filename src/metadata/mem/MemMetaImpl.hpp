// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed IMetaEngine implementation — thin facade around MemMetaStore
// that adds locking, file/dir handle accounting, and permission checks.

#pragma once

#include <mutex>
#include <string>

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaStore.hpp"

namespace swordfs::metadata {

class MemMetaImpl : public IMetaEngine {
 public:
  MemMetaImpl();
  ~MemMetaImpl() override;

  // Entry operations
  Status Lookup(InodeID parent_ino,
                std::string_view name, InodeID *child_ino,
                struct stat *attr) override;
  Status GetAttr(InodeID ino, struct stat *attr) override;
  Status Create(InodeID parent_ino,
                std::string_view name, mode_t mode, InodeID *child_ino,
                struct stat *attr) override;
  Status Unlink(InodeID parent_ino, std::string_view name) override;
  Status Rename(InodeID old_parent_ino,
                std::string_view old_name, InodeID new_parent_ino,
                std::string_view new_name, unsigned int flags) override;
  Status SetAttr(InodeID ino,
                 const struct stat *attr, int to_set,
                 struct stat *out_attr) override;
  Status Access(InodeID ino, int mask) override;
  Status Open(InodeID ino) override;
  Status ReclaimInode(InodeID ino) override;
  Status ListChunks(InodeID ino, std::vector<ChunkMeta> *out) override;

  // Directory operations
  Status ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) override;
  Status MkDir(InodeID parent_ino,
               std::string_view name, mode_t mode, InodeID *child_ino,
               struct stat *attr) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status OpenDir(InodeID ino) override;
  Status Forget(InodeID ino, uint64_t nlookup) override;

  // Link / symlink operations
  Status Symlink(InodeID parent_ino,
                 std::string_view name, const char *link,
                 InodeID *child_ino, struct stat *attr) override;
  Status Link(InodeID ino, InodeID newparent_ino,
              std::string_view newname, struct stat *attr) override;
  Status Readlink(InodeID ino, std::string *target) override;

  // Chunk metadata
  Status AddChunk(InodeID ino, const ChunkMeta &cm) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta *cm) override;
  Status Truncate(InodeID ino, size_t size) override;

  // Volume operations
  Status StatFs(struct statvfs *stbuf) override;

  static Limits GetLimits();

 private:
  // Helpers
  void KillSUID(struct stat *st);

 private:
  MemMetaStore store_;
};

}  // namespace swordfs::metadata
