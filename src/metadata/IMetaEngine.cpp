// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/IMetaEngine.hpp"

#include "metadata/Utils.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"

namespace swordfs::metadata {

Limits IMetaEngine::GetLimits(std::string_view meta_url) {
  std::string scheme;
  if (!ParseUrlScheme(meta_url, &scheme).ok()) {
    return Limits{};
  }
  if (scheme == "memory") {
    return MemMetaImpl::GetLimits();
  }
  if (scheme == "redis") {
    return RedisMetaImpl::GetLimits();
  }
  return Limits{};
}

}  // namespace swordfs::metadata
