// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaConfig.hpp"

#include <folly/Conv.h>
#include <folly/Uri.h>

#include <charconv>
#include <limits>

namespace swordfs::metadata {
namespace {

constexpr std::string_view kRedisScheme = "redis";

utils::Status ParsePort(std::string_view authority, RedisMetaConfig *config) {
  if (authority.empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }

  const auto at = authority.rfind('@');
  if (at != std::string_view::npos) {
    authority.remove_prefix(at + 1);
  }

  std::string_view port;
  if (authority.front() == '[') {
    const auto closing = authority.find(']');
    if (closing == std::string_view::npos) {
      return utils::Status::InvalidArgument("Redis metadata URL has invalid IPv6 host");
    }
    port = authority.substr(closing + 1);
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      port = authority.substr(colon);
    }
  }

  if (port.empty()) {
    return utils::Status::OK();
  }
  if (!port.starts_with(':')) {
    return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
  }

  const auto parsed_port = folly::tryTo<uint16_t>(port.substr(1));
  if (!parsed_port.hasValue() || parsed_port.value() == 0) {
    return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
  }
  config->port = parsed_port.value();
  return utils::Status::OK();
}

utils::Status ParseDuration(std::string_view value, std::chrono::milliseconds *duration) {
  if (value.empty() || value.front() == '-') {
    return utils::Status::InvalidArgument("Redis metadata duration is invalid");
  }

  std::string_view number = value;
  auto multiplier = std::chrono::milliseconds(1);
  if (value.ends_with("ms")) {
    number = value.substr(0, value.size() - 2);
  } else if (value.ends_with('s')) {
    number = value.substr(0, value.size() - 1);
    multiplier = std::chrono::seconds(1);
  } else if (value.ends_with('m')) {
    number = value.substr(0, value.size() - 1);
    multiplier = std::chrono::minutes(1);
  } else {
    return utils::Status::InvalidArgument("Redis metadata duration must use ms, s, or m");
  }

  int64_t parsed = 0;
  const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), parsed);
  if (number.empty() || error != std::errc{} || end != number.data() + number.size() || parsed <= 0) {
    return utils::Status::InvalidArgument("Redis metadata duration is invalid");
  }
  const auto multiplier_count = multiplier.count();
  if (parsed > std::chrono::milliseconds::max().count() / multiplier_count) {
    return utils::Status::InvalidArgument("Redis metadata duration is too large");
  }
  *duration = multiplier * parsed;
  return utils::Status::OK();
}

utils::Status ParsePositiveInteger(std::string_view value, std::string_view name, int64_t *result) {
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), *result);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size() || *result <= 0) {
    return utils::Status::InvalidArgument("Redis metadata option " + std::string(name) + " is invalid");
  }
  return utils::Status::OK();
}

utils::Status ParseQuery(std::string_view query, RedisMetaConfig *config) {
  while (!query.empty()) {
    const auto separator = query.find('&');
    const auto item = query.substr(0, separator);
    query = separator == std::string_view::npos ? std::string_view{} : query.substr(separator + 1);

    const auto equals = item.find('=');
    if (equals == std::string_view::npos) {
      return utils::Status::InvalidArgument("Redis metadata query option is missing a value");
    }
    const auto key = item.substr(0, equals);
    const auto value = item.substr(equals + 1);
    if (key.empty() || value.empty()) {
      return utils::Status::InvalidArgument("Redis metadata query option is invalid");
    }

    if (key == "connect_timeout") {
      auto status = ParseDuration(value, &config->connect_timeout);
      if (!status.ok()) {
        return status;
      }
    } else if (key == "socket_timeout") {
      auto status = ParseDuration(value, &config->socket_timeout);
      if (!status.ok()) {
        return status;
      }
    } else if (key == "pool_wait_timeout") {
      auto status = ParseDuration(value, &config->pool_wait_timeout);
      if (!status.ok()) {
        return status;
      }
    } else if (key == "retry_backoff") {
      auto status = ParseDuration(value, &config->retry_backoff);
      if (!status.ok()) {
        return status;
      }
    } else if (key == "session_timeout") {
      auto status = ParseDuration(value, &config->session_timeout);
      if (!status.ok()) {
        return status;
      }
    } else if (key == "pool_size") {
      int64_t parsed = 0;
      auto status = ParsePositiveInteger(value, key, &parsed);
      if (!status.ok()) {
        return status;
      }
      if (static_cast<uint64_t>(parsed) > std::numeric_limits<std::size_t>::max()) {
        return utils::Status::InvalidArgument("Redis metadata option pool_size is invalid");
      }
      config->pool_size = static_cast<std::size_t>(parsed);
    } else if (key == "retry_attempts") {
      int64_t parsed = 0;
      auto status = ParsePositiveInteger(value, key, &parsed);
      if (!status.ok()) {
        return status;
      }
      if (parsed > std::numeric_limits<int>::max()) {
        return utils::Status::InvalidArgument("Redis metadata option retry_attempts is invalid");
      }
      config->retry_attempts = static_cast<int>(parsed);
    } else {
      return utils::Status::InvalidArgument("Unknown Redis metadata option: " + std::string(key));
    }
  }
  return utils::Status::OK();
}

utils::Status ParseDatabase(std::string_view path, RedisMetaConfig *config) {
  if (path.empty()) {
    return utils::Status::OK();
  }
  if (!path.starts_with('/')) {
    return utils::Status::InvalidArgument("Redis metadata URL has invalid database path");
  }

  path.remove_prefix(1);
  if (path.empty()) {
    return utils::Status::OK();
  }

  const auto db = folly::tryTo<int>(path);
  if (!db.hasValue() || db.value() < 0) {
    return utils::Status::InvalidArgument("Redis metadata URL has invalid database");
  }
  config->db = db.value();
  return utils::Status::OK();
}

utils::Status ParseAuthority(const folly::Uri &uri, std::string_view meta_url, RedisMetaConfig *config) {
  auto authority = meta_url.substr(std::string_view("redis://").size());
  const auto authority_end = authority.find_first_of("/?#");
  authority = authority.substr(0, authority_end);

  auto status = ParsePort(authority, config);
  if (!status.ok()) {
    return status;
  }

  if (!uri.username().empty()) {
    config->username = uri.username();
  }
  if (!uri.password().empty()) {
    config->password = uri.password();
  }
  return utils::Status::OK();
}

}  // namespace

// Accepted form: redis://[username:password@]host[:port][/db]
// For example: redis://user:secret@example.com:6380/3.
utils::Status ParseRedisMetaUrl(std::string_view meta_url, RedisMetaConfig *config) {
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis metadata config is null");
  }

  const auto uri_result = folly::Uri::tryFromString(meta_url);
  if (!uri_result.hasValue()) {
    return utils::Status::InvalidArgument("Redis metadata URL is malformed");
  }
  const auto &uri = uri_result.value();
  if (uri.scheme() != kRedisScheme) {
    return utils::Status::InvalidArgument("Redis metadata URL must use redis scheme");
  }
  if (uri.host().empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }
  if (!uri.fragment().empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL does not support fragment components");
  }

  RedisMetaConfig parsed;
  parsed.host = uri.hostname();

  auto status = ParseAuthority(uri, meta_url, &parsed);
  if (!status.ok()) {
    return status;
  }
  status = ParseDatabase(uri.path(), &parsed);
  if (!status.ok()) {
    return status;
  }
  status = ParseQuery(uri.query(), &parsed);
  if (!status.ok()) {
    return status;
  }

  *config = std::move(parsed);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
