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
class RedisMetaClient;
class RedisMetaTxn;

// Redis-backed SwordFS metadata operations.
//
// RedisMetaImpl owns POSIX policy and composes these operations. This class
// owns the Redis metadata schema boundary: callers work with SwordFS metadata
// objects rather than Redis keys or serialized values. Complete metadata
// operations hide whether they need a direct Redis command or an internal
// transaction; RedisMetaImpl uses Transact() only when it must compose multiple
// transaction-scoped primitives into one atomic POSIX operation.
class RedisMetaOps {
 public:
  RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name);

  utils::Status Initialize();
  utils::Status FormatVolume(const SwordFsVolume &config);
  utils::Status LoadVolume(SwordFsVolume *config);
  utils::Status CreateDirIterator(InodeID ino, std::vector<SwordFsEntry> prefix_entries,
                                  std::shared_ptr<DirIterator> *iterator);

  // Standalone metadata operations. Each method owns any Redis transaction
  // needed to preserve its own consistency semantics.
  utils::Status GetInode(InodeID ino, SwordFsInode *out);
  utils::Status LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out);
  utils::Status SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out = nullptr);
  utils::Status Truncate(InodeID ino, uint64_t size);
  utils::Status TouchInode(InodeID ino, SetAttrField fields);
  utils::Status ReclaimInode(InodeID ino);
  utils::Status AddChunk(InodeID ino, const SwordFsChunk &chunk);
  utils::Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);
  utils::Status VisitChunks(InodeID ino, const std::function<utils::Status(const SwordFsChunk &)> &visitor);
  utils::Status GetInodeCount(uint64_t *count);
  utils::Status AllocateInode(InodeID *ino);

  // Run one optimistic metadata transaction when RedisMetaImpl must compose
  // multiple metadata primitives into one atomic POSIX operation. Standalone
  // metadata operations should be exposed as RedisMetaOps methods instead of
  // making callers open a transaction just to invoke one primitive.
  utils::Status Transact(const std::function<utils::Status(RedisMetaTxn &)> &callback);

 private:
  std::shared_ptr<RedisMetaClient> client_;
  redis::RedisKey key_;
  uint64_t chunk_size_ = 0;
};

}  // namespace swordfs::metadata
