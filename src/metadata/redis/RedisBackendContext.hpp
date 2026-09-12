// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <memory>

#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::utils {
class BlockingExecutor;
}

namespace swordfs::metadata {

class RedisMetaClient;

// Shared lifetime unit for the Redis synchronous client and its blocking
// executor. RedisMetaOps owns shutdown; iterators may keep the context object
// alive after that point, but Shutdown() releases all thread-domain resources
// before the metadata engine itself is destroyed.
class RedisBackendContext {
 public:
  explicit RedisBackendContext(const RedisMetaConfig &config, size_t worker_count);
  ~RedisBackendContext();

  RedisBackendContext(const RedisBackendContext &) = delete;
  RedisBackendContext &operator=(const RedisBackendContext &) = delete;

  RedisMetaClient &client() const;
  utils::BlockingExecutor &executor() const;

  void Shutdown();

 private:
  std::unique_ptr<RedisMetaClient> client_;
  std::unique_ptr<utils::BlockingExecutor> executor_;
};

}  // namespace swordfs::metadata
