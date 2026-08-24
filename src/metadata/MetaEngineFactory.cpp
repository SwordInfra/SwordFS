// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineFactory.hpp"

#include <exception>
#include <string>

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"

namespace swordfs::metadata {

utils::Status CreateMetaEngine(std::string_view scheme, std::string_view meta_url, std::unique_ptr<IMetaEngine> *out) {
  if (scheme == "memory") {
    *out = std::make_unique<MemMetaImpl>();
    return utils::Status::OK();
  }
  if (scheme == "redis") {
    RedisMetaConfig config;
    auto status = ParseRedisMetaUrl(meta_url, &config);
    if (!status.ok()) {
      return status;
    }
    try {
      auto redis = std::make_unique<RedisMetaImpl>(config);
      status = redis->Initialize();
      if (!status.ok()) {
        return status;
      }
      *out = std::move(redis);
    } catch (const std::exception &error) {
      return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
    }
    return utils::Status::OK();
  }
  return utils::Status::NotSupported("unsupported metadata engine: " + std::string(meta_url));
}

}  // namespace swordfs::metadata
