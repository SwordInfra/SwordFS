// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Synchronous Redis adapter. This component is intentionally thread-domain
// only: callers must perform any fiber -> thread transition before invoking it.
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

  // Runs one optimistic transaction entirely on the calling POSIX thread.
  // WATCH, reads, MULTI, queued writes and EXEC share one Redis connection.
  utils::Status Transact(const std::function<utils::Status(RedisKvTxn &)> &callback);

 private:
  std::unique_ptr<sw::redis::Redis> redis_;
  int retry_attempts_;
  std::chrono::milliseconds retry_backoff_;
};

}  // namespace swordfs::metadata
