// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

struct RedisMetaConfig {
  std::string host;
  uint16_t port = 6379;
  std::optional<std::string> username;
  std::optional<std::string> password;
  int db = 0;
};

bool IsRedisMetaUrl(std::string_view meta_url);
utils::Status ParseRedisMetaUrl(std::string_view meta_url,
                                RedisMetaConfig* config);

}  // namespace swordfs::metadata
