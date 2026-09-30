// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
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

 private:
  std::shared_ptr<RedisBackendContext> backend_;
  redis::RedisKey key_;
};

}  // namespace swordfs::metadata
