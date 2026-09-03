// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class DirIterator;
class RedisKvTxn;
class RedisMetaClient;

// Redis-backed SwordFS metadata operations.
//
// RedisMetaImpl owns POSIX policy and composes these operations. This class
// owns the Redis metadata schema boundary: callers work with SwordFS metadata
// objects rather than Redis keys or serialized values. Simple reads use the
// direct Redis client path, while compound mutations are composed through Txn.
class RedisMetaOps {
 public:
  class Txn {
   public:
    Txn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size);

    // ────────────────────────────────────────────────────────────────
    // Reads
    // ────────────────────────────────────────────────────────────────
    utils::Status LookupInode(InodeID ino, SwordFsInode *out);
    utils::Status LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out);
    utils::Status EntryExists(InodeID parent_ino, std::string_view name, bool *exists);
    utils::Status IsDirEmpty(InodeID ino, bool *empty);
    utils::Status IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result);

    // ────────────────────────────────────────────────────────────────
    // Inode primitives
    // ────────────────────────────────────────────────────────────────
    utils::Status InsertInode(const SwordFsInode &inode);
    utils::Status SetInode(const SwordFsInode &inode);
    utils::Status DeleteInode(InodeID ino);
    utils::Status AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);

    // ────────────────────────────────────────────────────────────────
    // Directory-entry primitives
    // ────────────────────────────────────────────────────────────────
    utils::Status LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
    utils::Status UnlinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                              SwordFsInode *parent);
    utils::Status ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                               SwordFsInode *parent);
    utils::Status DeleteDirectory(InodeID ino);
    utils::Status AdjustInodeCount(int64_t delta);

    // ────────────────────────────────────────────────────────────────
    // Chunk primitives
    // ────────────────────────────────────────────────────────────────
    utils::Status SetChunk(InodeID ino, const SwordFsChunk &chunk);
    utils::Status TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size);
    utils::Status DeleteChunks(InodeID ino);

   private:
    utils::Status GetEntry(InodeID parent_ino, std::string_view name, SwordFsEntry *out);
    utils::Status LookupChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);

   private:
    RedisKvTxn &txn_;
    const redis::RedisKey &key_;
    uint64_t chunk_size_;
  };

  RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name);

  utils::Status Initialize();
  utils::Status FormatVolume(const SwordFsVolume &config);
  utils::Status LoadVolume(SwordFsVolume *config);
  utils::Status CreateDirIterator(InodeID ino, std::vector<SwordFsEntry> prefix_entries,
                                  std::shared_ptr<DirIterator> *iterator);

  // Direct, non-transactional metadata operations.
  utils::Status GetInode(InodeID ino, SwordFsInode *out);
  utils::Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);
  utils::Status VisitChunks(InodeID ino, const std::function<utils::Status(const SwordFsChunk &)> &visitor);
  utils::Status GetInodeCount(uint64_t *count);
  utils::Status AllocateInode(InodeID *ino);

  // Run one optimistic metadata transaction. The callback composes stateless
  // metadata primitives; intermediate POSIX operation state stays in caller
  // locals and is passed explicitly between operations.
  utils::Status Transact(const std::function<utils::Status(Txn &)> &callback);

 private:
  std::shared_ptr<RedisMetaClient> client_;
  redis::RedisKey key_;
  uint64_t chunk_size_ = 0;
};

}  // namespace swordfs::metadata
