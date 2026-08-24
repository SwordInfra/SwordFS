// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

struct RedisMetaConfig {
  // Redis endpoint and authentication.
  std::string host;
  uint16_t port = 6379;
  int db = 0;
  std::optional<std::string> username;
  std::optional<std::string> password;

  // Connection timeouts.
  std::chrono::milliseconds connect_timeout = std::chrono::seconds(2);
  std::chrono::milliseconds socket_timeout = std::chrono::seconds(5);

  // Connection pool limits and wait policy.
  std::size_t pool_size = 8;
  std::chrono::milliseconds pool_wait_timeout = std::chrono::seconds(1);

  // Transaction retry policy.
  int retry_attempts = 3;
  std::chrono::milliseconds retry_backoff = std::chrono::milliseconds(20);
};

utils::Status ParseRedisMetaUrl(std::string_view meta_url, RedisMetaConfig *config);

}  // namespace swordfs::metadata
