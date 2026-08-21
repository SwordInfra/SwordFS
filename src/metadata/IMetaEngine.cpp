// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"

namespace swordfs::metadata {

Limits IMetaEngine::GetLimits(std::string_view meta_url) {
  if (IsMemoryMode(meta_url)) {
    return MemMetaImpl::GetLimits();
  }
  if (IsRedisMetaUrl(meta_url)) {
    return RedisMetaImpl::GetLimits();
  }
  return Limits{};
}

}  // namespace swordfs::metadata
