// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <functional>
#include <memory>

#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaStore {
public:
  static constexpr int kDefaultMaxAttempts = 8;

  explicit RedisMetaStore(const RedisMetaConfig &config);

  RedisMetaStore(const RedisMetaStore &) = delete;
  RedisMetaStore &operator=(const RedisMetaStore &) = delete;

  utils::Status Ping();

  // Runs an optimistic transaction. Every attempt owns one dedicated Redis
  // connection so WATCH, reads, MULTI, queued writes and EXEC share the same
  // connection. Busy and WATCH conflicts retry with bounded backoff.
  //
  utils::Status Transact(const std::function<utils::Status(RedisMetaTxn &)> &callback, int max_attempts = kDefaultMaxAttempts);

private:
  std::unique_ptr<sw::redis::Redis> redis_;
};

}  // namespace swordfs::metadata
