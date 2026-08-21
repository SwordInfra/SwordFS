// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <functional>
#include <memory>

#include <sw/redis++/redis++.h>

#include "metadata/redis/RedisMetaConfig.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaStore {
 public:
  static constexpr int kDefaultMaxAttempts = 8;

  explicit RedisMetaStore(const RedisMetaConfig& config);

  RedisMetaStore(const RedisMetaStore&) = delete;
  RedisMetaStore& operator=(const RedisMetaStore&) = delete;

  utils::Status Ping();

  // Runs one optimistic transaction attempt at a time. The callback must only
  // queue writes after calling MULTI and must return Busy to request a retry.
  // Connection failures before EXEC are retried. A failure while waiting for an
  // EXEC response is intentionally returned as an ambiguous commit.
  utils::Status Transact(
      const std::function<utils::Status(sw::redis::Transaction&)>& callback,
      int max_attempts = kDefaultMaxAttempts);

 private:
  std::unique_ptr<sw::redis::Redis> redis_;
};

}  // namespace swordfs::metadata
