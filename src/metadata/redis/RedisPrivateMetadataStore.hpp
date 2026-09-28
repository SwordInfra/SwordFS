// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "metadata/IPrivateMetadata.hpp"
#include "metadata/redis/RedisKey.hpp"

namespace swordfs::metadata {

class RedisBackendContext;

// Mechanism-scoped Redis capability. Concrete mechanism adapters derive from
// this class and own typed record encoding; this base owns only private
// sequence allocation and keeps physical key construction inside Redis.
class RedisPrivateMetadataStore : public IMechanismPrivateStore {
 public:
  RedisPrivateMetadataStore(std::shared_ptr<RedisBackendContext> backend, redis::RedisKey key,
                            ChunkOverwriteMechanism mechanism)
      : backend_(std::move(backend)), key_(std::move(key)), mechanism_(mechanism) {
  }

  ChunkOverwriteMechanism mechanism() const override {
    return mechanism_;
  }

 private:
  utils::Status AllocateSequenceImpl(uint32_t stable_id, uint64_t *value) override;

 private:
  std::shared_ptr<RedisBackendContext> backend_;
  redis::RedisKey key_;
  ChunkOverwriteMechanism mechanism_;
};

}  // namespace swordfs::metadata
