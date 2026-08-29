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
  Status Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) override;
  Status GetInode(InodeID ino, SwordFsInode *out) override;
  Status Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status Unlink(InodeID parent_ino, std::string_view name, uint64_t *post_nlink = nullptr) override;
  Status Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino, std::string_view new_name,
                RenameFlag flags, RenameResult *result = nullptr) override;
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override;
  Status Access(InodeID ino, uint32_t mask) override;
  Status Open(InodeID ino) override;
  Status ReclaimInode(InodeID ino) override;

  // Directory operations
  Status MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status OpenDir(InodeID ino, DirIteratorPtr *iterator) override;

  // Link / symlink operations
  Status Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) override;
  Status Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) override;
  Status Readlink(InodeID ino, std::string *target) override;

  // Chunk metadata
  Status AddChunk(InodeID ino, const SwordFsChunk &chunk) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) override;
  Status ListChunks(InodeID ino, std::vector<SwordFsChunk> *out) override;
  Status Truncate(InodeID ino, uint64_t size) override;

  // Volume operations
  Status Initialize() override;
  Status FormatVolume(const SwordFsVolume &config) override;
  Status LoadVolume(SwordFsVolume *config) override;
  Status StatFs(SwordFsStatFs *stbuf) override;

  Limits GetLimits() const override;

 private:
  MemMetaStore store_;
};

}  // namespace swordfs::metadata
