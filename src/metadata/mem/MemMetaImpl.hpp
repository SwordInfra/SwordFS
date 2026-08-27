// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed IMetaEngine implementation — thin facade around MemMetaStore
// that adds file/dir handle accounting and permission checks.  Transactions
// are owned by the store: each method here runs as a single
// MemMetaStore::Transact() script so every IMetaEngine operation is atomic.

#pragma once

#include <mutex>
#include <string>

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaStore.hpp"

namespace swordfs::metadata {

class MemMetaImpl : public IMetaEngine {
 public:
  MemMetaImpl() = default;
  ~MemMetaImpl() override = default;

  // Entry operations
  Status Lookup(InodeID parent_ino, std::string_view name,
                SwordFsInode *out) override;
  Status GetInode(InodeID ino, SwordFsInode *out) override;
  Status Create(InodeID parent_ino, std::string_view name, mode_t mode,
                SwordFsInode *out) override;
  Status Unlink(InodeID parent_ino, std::string_view name,
                nlink_t *post_nlink = nullptr) override;
  Status Rename(InodeID old_parent_ino,
                std::string_view old_name, InodeID new_parent_ino,
                std::string_view new_name, RenameFlag flags,
                RenameResult *result = nullptr) override;
  Status SetAttr(InodeID ino, const struct stat *attr,
                 SetAttrField fields, SwordFsInode *out) override;
  Status Access(InodeID ino, int mask) override;
  Status Open(InodeID ino) override;
  Status ReclaimInode(InodeID ino) override;

  // Directory operations
  Status ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) override;
  Status MkDir(InodeID parent_ino, std::string_view name, mode_t mode,
               SwordFsInode *out) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status OpenDir(InodeID ino) override;

  // Link / symlink operations
  Status Symlink(InodeID parent_ino, std::string_view name,
                 const char *link, SwordFsInode *out) override;
  Status Link(InodeID ino, InodeID newparent_ino,
              std::string_view newname, SwordFsInode *out) override;
  Status Readlink(InodeID ino, std::string *target) override;

  // Chunk metadata
  Status AddChunk(InodeID ino, const SwordFsChunk &chunk) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) override;
  Status ListChunks(InodeID ino, std::vector<SwordFsChunk> *out) override;
  Status Truncate(InodeID ino, size_t size) override;

  // Volume operations
  Status FormatVolume(const SwordFsVolume &config) override;
  Status LoadVolume(SwordFsVolume *config) override;
  Status StatFs(struct statvfs *stbuf) override;

  Limits GetLimits() const override;

 private:
  MemMetaStore store_;
};

}  // namespace swordfs::metadata
