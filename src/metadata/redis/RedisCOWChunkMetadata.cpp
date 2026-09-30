// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisCOWChunkMetadata.hpp"

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

utils::Status RedisCOWChunkMetadata::AllocateChunkID(ChunkID *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("ChunkID output is null");
  }
  uint64_t value = 0;
  auto status =
      backend_->executor().RunFromFiber([&] { return backend_->client().IncrNonNegative(key_.NextChunkID(), &value); });
  if (!status.ok()) {
    return status;
  }
  *out = ChunkID(value);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
