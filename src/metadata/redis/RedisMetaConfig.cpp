// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaConfig.hpp"

#include <folly/Conv.h>
#include <folly/Uri.h>

#include "metadata/MetaEngineRegistry.hpp"

namespace swordfs::metadata {
namespace {

RegisterMetaEngine kRedisMetaEngine{"redis"};
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
  // TODO(#111): accept the volume query parameter once the Redis key schema
  // and volume namespace are implemented. Phase 0 intentionally rejects query
  // components rather than silently ignoring them.
  if (!uri.query().empty() || !uri.fragment().empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL does not support query or fragment components");
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

  *config = std::move(parsed);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
