// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "metadata/cow/COWChunkMetadata.hpp"
#include "metadata/redis/RedisKey.hpp"

namespace swordfs::metadata {

class RedisBackendContext;

// Redis implementation of the independent COW ChunkMetadata domain. It shares
// backend connection/executor infrastructure but never a RedisMetaTxn
// correctness boundary.
class RedisCOWChunkMetadata final : public cow::COWChunkMetadata {
 public:
  RedisCOWChunkMetadata(std::shared_ptr<RedisBackendContext> backend, redis::RedisKey key)
      : backend_(std::move(backend)), key_(std::move(key)) {
  }

  utils::Status AllocateChunkID(ChunkID *out) override;
  utils::Status AllocateRevision(ChunkID chunk_id, cow::COWChunkRevision *out) override;
  utils::Status GetHead(ChunkID chunk_id, cow::COWChunkHead *out) override;
  utils::Status CompareExchangeHead(ChunkID chunk_id, const std::optional<cow::COWChunkHead> &expected,
                                    const cow::COWChunkHead &replacement) override;
  utils::Status EraseHead(ChunkID chunk_id, const cow::COWChunkHead &expected) override;

 private:
  std::string HeadKey(ChunkID chunk_id) const;
  std::string RevisionKey(ChunkID chunk_id) const;

 private:
  std::shared_ptr<RedisBackendContext> backend_;
  redis::RedisKey key_;
};

}  // namespace swordfs::metadata
