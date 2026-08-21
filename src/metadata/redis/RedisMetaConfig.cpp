// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaConfig.hpp"

#include <charconv>
#include <climits>
#include <limits>

namespace swordfs::metadata {
namespace {

constexpr std::string_view kRedisScheme = "redis://";

bool ParseUnsigned(std::string_view value, uint32_t* out) {
  if (value.empty()) {
    return false;
  }
  uint32_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
    return false;
  }
  *out = parsed;
  return true;
}

}  // namespace

bool IsRedisMetaUrl(std::string_view meta_url) {
  return meta_url.starts_with(kRedisScheme);
}

utils::Status ParseRedisMetaUrl(std::string_view meta_url,
                                RedisMetaConfig* config) {
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis metadata config is null");
  }
  if (!IsRedisMetaUrl(meta_url)) {
    return utils::Status::InvalidArgument("Redis metadata URL must start with redis://");
  }

  auto authority_and_path = meta_url.substr(kRedisScheme.size());
  if (authority_and_path.empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }
  // TODO(#111): accept the volume query parameter once the Redis key schema
  // and volume namespace are implemented. Phase 0 intentionally rejects query
  // components rather than silently ignoring them.
  if (authority_and_path.find_first_of("?#") != std::string_view::npos) {
    return utils::Status::InvalidArgument(
        "Redis metadata URL does not support query or fragment components");
  }

  const auto slash = authority_and_path.find('/');
  const auto authority = authority_and_path.substr(0, slash);
  const auto path = slash == std::string_view::npos
                        ? std::string_view()
                        : authority_and_path.substr(slash + 1);
  if (authority.empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }

  RedisMetaConfig parsed;
  const auto at = authority.rfind('@');
  auto host_and_port = authority;
  if (at != std::string_view::npos) {
    const auto userinfo = authority.substr(0, at);
    host_and_port = authority.substr(at + 1);
    const auto colon = userinfo.find(':');
    if (colon == std::string_view::npos) {
      parsed.password = std::string(userinfo);
    } else {
      if (colon != 0) {
        parsed.username = std::string(userinfo.substr(0, colon));
      }
      parsed.password = std::string(userinfo.substr(colon + 1));
    }
  }

  if (host_and_port.empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }
  if (host_and_port.front() == '[') {
    const auto closing = host_and_port.find(']');
    if (closing == std::string_view::npos) {
      return utils::Status::InvalidArgument("Redis metadata URL has invalid IPv6 host");
    }
    parsed.host = std::string(host_and_port.substr(1, closing - 1));
    const auto suffix = host_and_port.substr(closing + 1);
    if (!suffix.empty()) {
      if (!suffix.starts_with(':')) {
        return utils::Status::InvalidArgument("Redis metadata URL has invalid IPv6 host");
      }
      uint32_t port = 0;
      if (!ParseUnsigned(suffix.substr(1), &port) || port == 0 ||
          port > std::numeric_limits<uint16_t>::max()) {
        return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
      }
      parsed.port = static_cast<uint16_t>(port);
    }
  } else {
    const auto colon = host_and_port.rfind(':');
    if (colon == std::string_view::npos) {
      parsed.host = std::string(host_and_port);
    } else {
      parsed.host = std::string(host_and_port.substr(0, colon));
      uint32_t port = 0;
      if (!ParseUnsigned(host_and_port.substr(colon + 1), &port) || port == 0 ||
          port > std::numeric_limits<uint16_t>::max()) {
        return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
      }
      parsed.port = static_cast<uint16_t>(port);
    }
  }
  if (parsed.host.empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }

  if (!path.empty()) {
    uint32_t db = 0;
    if (!ParseUnsigned(path, &db) || db > static_cast<uint32_t>(INT_MAX)) {
      return utils::Status::InvalidArgument("Redis metadata URL has invalid database");
    }
    parsed.db = static_cast<int>(db);
  }

  *config = std::move(parsed);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
