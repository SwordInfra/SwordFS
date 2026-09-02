// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

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
  utils::Status Get(std::string_view key, std::string *value);
  utils::Status HGet(std::string_view key, std::string_view field, std::string *value);
  utils::Status Incr(std::string_view key, uint64_t *value);
  utils::Status HScan(std::string_view key, uint64_t cursor, size_t count,
                      std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor);

  // Runs an optimistic transaction. Every attempt exclusively uses one
  // connection checked out from the Redis client's connection pool, so WATCH,
  // reads, MULTI, queued writes and EXEC share the same connection. Busy and
  // WATCH conflicts retry with bounded backoff.
  utils::Status Transact(const std::function<utils::Status(RedisMetaTxn &)> &callback);

 private:
  utils::Status TransactImpl(const std::function<utils::Status(RedisMetaTxn &)> &callback);

 private:
  std::unique_ptr<sw::redis::Redis> redis_;
  std::unique_ptr<utils::FiberThreadPool> pool_;
  int retry_attempts_;
  std::chrono::milliseconds retry_backoff_;
};

}  // namespace swordfs::metadata
