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
  Status OpenChunkMetadata(ChunkType chunk_type, ChunkMetadataPtr *out) override;
  Status BindChunkMetadataBridge(chunk::internal::ChunkMetadataBridge *bridge) override;

  // Entry operations
  Status Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) override;
  Status GetInode(InodeID ino, SwordFsInode *out) override;
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override;
  Status Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status MkNod(InodeID parent_ino, std::string_view name, uint32_t mode, uint64_t rdev, SwordFsInode *out) override;
  Status Unlink(InodeID parent_ino, std::string_view name, std::optional<InodeID> expected_ino = std::nullopt) override;
  Status Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino, std::string_view new_name,
                RenameFlag flags) override;
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override;
  Status SetInodeFlags(InodeID ino, InodeFlag inode_flags, SwordFsInode *out) override;
  Status SetXAttr(InodeID ino, std::string_view name, std::string_view value, XAttrSetMode mode) override;
  Status GetXAttr(InodeID ino, std::string_view name, std::string *value) override;
  Status ListXAttrs(InodeID ino, std::vector<std::string> *names) override;
  Status RemoveXAttr(InodeID ino, std::string_view name) override;
  Status Open(InodeID ino, uint64_t *size = nullptr, InodeFlag *inode_flags = nullptr) override;
  Status PrepareReclaim(InodeID ino) override;
  Status CompleteReclaim(InodeID ino) override;
  Status VisitOrphanCandidates(const InodeVisitorFn &visitor) override;
  Status VisitPendingReclaims(const ReclaimVisitorFn &visitor) override;
  Status VisitPendingDeletesBatch(size_t max_items, const PendingDeleteVisitorFn &visitor, bool *has_more) override;
  Status CompletePendingDelete(std::string_view key) override;
  Status ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, FileChunkSnapshot *out) override;
  Status ReadFileMappingSnapshot(InodeID ino, FileMappingSnapshot *out) override;
  Status ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out) override;
  Status AttachPrepared(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                        const FileSizePrecondition &expected) override;
  Status FinalizeAttachedWrite(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                               const FileSizePrecondition &expected) override;
  Status CommitShrink(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested, SetAttrField fields,
                      ChunkSizeCommitResult *out) override;
  Status CommitGrow(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested, SetAttrField fields,
                    ChunkSizeCommitResult *out) override;
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
  Status CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement,
                     const ChunkPublishIntent &intent) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) override;
  Status LoadChunkView(InodeID ino, ChunkIndex idx, ChunkView *out) override;
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
  utils::FiberMutex orphan_scan_mutex_;
  std::vector<InodeID> orphan_scan_snapshot_;
  size_t orphan_scan_snapshot_offset_ = 0;
  utils::FiberMutex pending_delete_scan_mutex_;
  std::vector<PendingDelete> pending_delete_snapshot_;
  size_t pending_delete_snapshot_offset_ = 0;
};

}  // namespace swordfs::metadata
