// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaOpsContext.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class DirIterator;

// Redis-backed SwordFS metadata operation facade.
//
// RedisMetaImpl owns POSIX policy and composes only this API. Direct operations
// are delegated to raw_, while operations that participate in one optimistic
// transaction are delegated to txn_ through RedisMetaOpsContext.
class RedisMetaOps {
 public:
  using TransactFn = std::function<utils::Status(RedisMetaOpsContext &)>;

  RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name);

  utils::Status Initialize();
  utils::Status FormatVolume(const SwordFsVolume &config);
  utils::Status LoadVolume(SwordFsVolume *config);
  utils::Status CreateDirIterator(InodeID ino, std::vector<SwordFsEntry> prefix_entries,
                                  std::shared_ptr<DirIterator> *iterator);

  // ────────────────────────────────────────────────────────────────
  // Direct operations
  // ────────────────────────────────────────────────────────────────
  utils::Status GetInode(InodeID ino, SwordFsInode *out);
  utils::Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);
  utils::Status VisitChunks(InodeID ino, const std::function<utils::Status(const SwordFsChunk &)> &visitor);
  utils::Status GetInodeCount(uint64_t *count);
  utils::Status AllocateInode(InodeID *ino);

  // ────────────────────────────────────────────────────────────────
  // Transaction scope
  // ────────────────────────────────────────────────────────────────
  utils::Status Transact(const TransactFn &callback);

  // ────────────────────────────────────────────────────────────────
  // Transactional metadata operations
  // ────────────────────────────────────────────────────────────────
  utils::Status LookupInode(RedisMetaOpsContext &ctx, InodeID ino, SwordFsInode *out);
  utils::Status LookupEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name, SwordFsInode *out);
  utils::Status EntryExists(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name, bool *exists);
  utils::Status IsDirEmpty(RedisMetaOpsContext &ctx, InodeID ino, bool *empty);
  utils::Status IsDescendantOf(RedisMetaOpsContext &ctx, InodeID ancestor_ino, InodeID child_ino, bool *result);

  utils::Status InsertInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode);
  utils::Status SetInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode);
  utils::Status DeleteInode(RedisMetaOpsContext &ctx, InodeID ino);
  utils::Status AdjustNlink(RedisMetaOpsContext &ctx, SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);

  utils::Status LinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                          const SwordFsInode &child, SwordFsInode *parent);
  utils::Status UnlinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                            const SwordFsInode &target, SwordFsInode *parent);
  utils::Status ReplaceEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                             const SwordFsInode &child, SwordFsInode *parent);
  utils::Status DeleteDirectory(RedisMetaOpsContext &ctx, InodeID ino);
  utils::Status AdjustInodeCount(RedisMetaOpsContext &ctx, int64_t delta);

  utils::Status SetChunk(RedisMetaOpsContext &ctx, InodeID ino, const SwordFsChunk &chunk);
  utils::Status TruncateChunks(RedisMetaOpsContext &ctx, InodeID ino, uint64_t old_size, uint64_t new_size);
  utils::Status DeleteChunks(RedisMetaOpsContext &ctx, InodeID ino);

 private:
  RedisMetaClient raw_;
  redis::RedisKey key_;
  uint64_t chunk_size_ = 0;
  RedisMetaTxn txn_;
};

}  // namespace swordfs::metadata
