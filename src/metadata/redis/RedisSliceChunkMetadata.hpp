// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"

namespace swordfs::metadata {

class RedisBackendContext;

class RedisSliceChunkMetadata final : public slice::SliceChunkMetadata {
 public:
  RedisSliceChunkMetadata(std::shared_ptr<RedisBackendContext> backend, redis::RedisKey key, uint32_t chunk_size)
      : backend_(std::move(backend)), key_(std::move(key)), chunk_size_(chunk_size) {
  }

  utils::Status AllocateChunkID(ChunkID *out) override;
  utils::Status AllocateSliceID(chunk::slice::SliceID *out) override;
  utils::Status CreateIndex(ChunkID chunk_id) override;
  utils::Status ReadRecords(ChunkID chunk_id, std::vector<chunk::slice::SliceRecord> *out) override;
  utils::Status AppendRecord(ChunkID chunk_id, const chunk::slice::SliceRecord &record) override;

 private:
  std::shared_ptr<RedisBackendContext> backend_;
  redis::RedisKey key_;
  uint32_t chunk_size_;
};

}  // namespace swordfs::metadata
