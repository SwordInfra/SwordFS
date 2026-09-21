// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Memory-backed IMetaEngine implementation — thin facade around MemMetaStore
// that adds filesystem policy and operation-level validation. Transactions
// are owned by the store: each method here runs as a single
// MemMetaStore::Transact() script so every IMetaEngine operation is atomic.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {

class MemMetaImpl : public IMetaEngine {
 public:
  static utils::Status CreateInstance(std::string_view meta_url, std::string_view volume_name,
                                      std::unique_ptr<IMetaEngine> *out);

  MemMetaImpl();
  ~MemMetaImpl() override;

  // Entry operations
  Status Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) override;
  Status GetInode(InodeID ino, SwordFsInode *out) override;
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override;
  Status Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status MkNod(InodeID parent_ino, std::string_view name, uint32_t mode, uint64_t rdev, SwordFsInode *out) override;
  Status Unlink(InodeID parent_ino, std::string_view name) override;
  Status Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino, std::string_view new_name,
                RenameFlag flags) override;
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override;
  Status Open(InodeID ino) override;
  Status PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) override;
  Status CompleteReclaim(InodeID ino) override;
  Status VisitOrphanCandidates(const InodeVisitorFn &visitor) override;
  Status VisitPendingReclaims(const ReclaimVisitorFn &visitor) override;
  Status VisitPendingDeletesBatch(size_t max_items, const PendingDeleteVisitorFn &visitor, bool *has_more) override;
  Status CompletePendingDelete(std::string_view key) override;
  Status AllocateChunkRevision(ChunkRevision *revision) override;

  // Directory operations
  Status MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status OpenDir(InodeID ino, DirIteratorPtr *iterator) override;

  // Link / symlink operations
  Status Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) override;
  Status Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) override;
  Status Readlink(InodeID ino, std::string *target) override;

  // Chunk metadata
  Status CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                     const SwordFsChunk &replacement) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) override;
  Status Truncate(InodeID ino, uint64_t size) override;

  // Volume operations
  Status Initialize() override;
  Status FormatVolume(const SwordFsVolume &config) override;
  Status LoadVolume(SwordFsVolume *config) override;
  Status StatFs(SwordFsStatFs *stbuf) override;

  Limits GetLimits() const override;

 private:
  MemMetaStore store_;
  uint64_t chunk_size_ = SwordFsVolume{}.chunk_size;
  utils::FiberMutex pending_delete_scan_mutex_;
  std::vector<PendingDelete> pending_delete_snapshot_;
  size_t pending_delete_snapshot_offset_ = 0;
};

}  // namespace swordfs::metadata
