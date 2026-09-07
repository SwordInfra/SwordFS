// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaOpsContext.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaClient;
class RedisMetaOps;

// Transactional implementation behind RedisMetaOps. This object is long-lived;
// each Transact() call creates a short-lived RedisMetaOpsContext for one
// optimistic Redis transaction attempt.
class RedisMetaTxn {
 public:
  RedisMetaTxn(RedisMetaClient &raw, const redis::RedisKey &key, const uint64_t &chunk_size);

 private:
  friend class RedisMetaOps;

  using TransactFn = std::function<utils::Status(RedisMetaOpsContext &)>;

  utils::Status Transact(const TransactFn &callback);
  utils::Status FormatVolume(const SwordFsVolume &config);

  // ────────────────────────────────────────────────────────────────
  // Reads
  // ────────────────────────────────────────────────────────────────
  utils::Status LookupInode(RedisMetaOpsContext &ctx, InodeID ino, SwordFsInode *out);
  utils::Status LookupEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name, SwordFsInode *out);
  utils::Status EntryExists(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name, bool *exists);
  utils::Status IsDirEmpty(RedisMetaOpsContext &ctx, InodeID ino, bool *empty);
  utils::Status IsDescendantOf(RedisMetaOpsContext &ctx, InodeID ancestor_ino, InodeID child_ino, bool *result);

  // ────────────────────────────────────────────────────────────────
  // Inode primitives
  // ────────────────────────────────────────────────────────────────
  utils::Status InsertInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode);
  utils::Status SetInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode);
  utils::Status DeleteInode(RedisMetaOpsContext &ctx, InodeID ino);
  utils::Status AdjustNlink(RedisMetaOpsContext &ctx, SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);

  // ────────────────────────────────────────────────────────────────
  // Directory-entry primitives
  // ────────────────────────────────────────────────────────────────
  utils::Status LinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                          const SwordFsInode &child, SwordFsInode *parent);
  utils::Status UnlinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                            const SwordFsInode &target, SwordFsInode *parent);
  utils::Status ReplaceEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                             const SwordFsInode &child, SwordFsInode *parent);
  utils::Status DeleteDirectory(RedisMetaOpsContext &ctx, InodeID ino);
  utils::Status AdjustInodeCount(RedisMetaOpsContext &ctx, int64_t delta);

  // ────────────────────────────────────────────────────────────────
  // Chunk primitives
  // ────────────────────────────────────────────────────────────────
  utils::Status SetChunk(RedisMetaOpsContext &ctx, InodeID ino, const SwordFsChunk &chunk);
  utils::Status TruncateChunks(RedisMetaOpsContext &ctx, InodeID ino, uint64_t old_size, uint64_t new_size);
  utils::Status DeleteChunks(RedisMetaOpsContext &ctx, InodeID ino);

 private:
  utils::Status GetEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name, SwordFsEntry *out);
  utils::Status LookupChunk(RedisMetaOpsContext &ctx, InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);

 private:
  RedisMetaClient &raw_;
  const redis::RedisKey &key_;
  const uint64_t &chunk_size_;
};

}  // namespace swordfs::metadata
