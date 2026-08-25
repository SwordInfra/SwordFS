// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <chrono>
#include <functional>
#include <memory>

#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "utils/Status.hpp"

namespace swordfs::utils {
class FiberThreadPool;
}  // namespace swordfs::utils

namespace swordfs::metadata {

class RedisMetaClient {
 public:
  explicit RedisMetaClient(const RedisMetaConfig &config);
  ~RedisMetaClient();

  RedisMetaClient(const RedisMetaClient &) = delete;
  RedisMetaClient &operator=(const RedisMetaClient &) = delete;

  utils::Status Ping();

  // Runs an optimistic transaction. Every attempt exclusively uses one
  // connection checked out from the Redis client's connection pool, so WATCH,
  // reads, MULTI, queued writes and EXEC share the same connection. Busy and
  // WATCH conflicts retry with bounded backoff.
  utils::Status Transact(const std::function<utils::Status(RedisMetaTxn &)> &callback);

 private:
  utils::Status TransactImpl(const std::function<utils::Status(RedisMetaTxn &)> &callback);
  std::unique_ptr<sw::redis::Redis> redis_;
  std::unique_ptr<utils::FiberThreadPool> pool_;
  int retry_attempts_;
  std::chrono::milliseconds retry_backoff_;
};

}  // namespace swordfs::metadata
