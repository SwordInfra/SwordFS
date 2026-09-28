// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisPrivateMetadataStore.hpp"

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

utils::Status RedisPrivateMetadataStore::AllocateSequenceImpl(uint32_t stable_id, uint64_t *value) {
  utils::ExpectInFiberDomain();
  const auto sequence_key = key_.PrivateSequence(mechanism_, stable_id);
  return backend_->executor().RunFromFiber([&] { return backend_->client().IncrNonNegative(sequence_key, value); });
}

}  // namespace swordfs::metadata
