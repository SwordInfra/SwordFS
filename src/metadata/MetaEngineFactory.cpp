// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineFactory.hpp"

#include <string>

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"

namespace swordfs::metadata {

utils::Status CreateMetaEngine(std::string_view meta_url,
                               std::unique_ptr<IMetaEngine>* out) {
  if (IsMemoryMode(meta_url)) {
    *out = std::make_unique<MemMetaImpl>();
    return utils::Status::OK();
  }
  if (IsRedisMetaUrl(meta_url)) {
    RedisMetaConfig config;
    auto status = ParseRedisMetaUrl(meta_url, &config);
    if (!status.ok()) {
      return status;
    }
    std::unique_ptr<RedisMetaImpl> redis;
    status = RedisMetaImpl::Create(config, &redis);
    if (!status.ok()) {
      return status;
    }
    *out = std::move(redis);
    return utils::Status::OK();
  }
  return utils::Status::NotSupported(
      "unsupported metadata engine: " + std::string(meta_url));
}

}  // namespace swordfs::metadata
