// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaConfig.hpp"

#include <folly/Conv.h>
#include <folly/Uri.h>

#include "metadata/MetaEngineRegistry.hpp"

namespace swordfs::metadata {
namespace {

RegisterMetaEngine kRedisMetaEngine{"redis", "redis://host[:port][/db]"};
constexpr std::string_view kRedisScheme = "redis://";

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

  folly::Expected<folly::Uri, folly::UriFormatError> uri_result =
      folly::Uri::tryFromString(meta_url);
  if (!uri_result.hasValue()) {
    return utils::Status::InvalidArgument("Redis metadata URL is malformed");
  }
  const auto& uri = uri_result.value();
  if (uri.scheme() != "redis") {
    return utils::Status::InvalidArgument("Redis metadata URL must use redis scheme");
  }
  if (uri.host().empty()) {
    return utils::Status::InvalidArgument("Redis metadata URL has no host");
  }
  // TODO(#111): accept the volume query parameter once the Redis key schema
  // and volume namespace are implemented. Phase 0 intentionally rejects query
  // components rather than silently ignoring them.
  if (!uri.query().empty() || !uri.fragment().empty()) {
    return utils::Status::InvalidArgument(
        "Redis metadata URL does not support query or fragment components");
  }

  RedisMetaConfig parsed;
  parsed.host = uri.hostname();
  auto authority = meta_url.substr(kRedisScheme.size());
  const auto authority_end = authority.find_first_of("/?#");
  authority = authority.substr(0, authority_end);
  const auto at = authority.rfind('@');
  if (at != std::string_view::npos) {
    authority.remove_prefix(at + 1);
  }

  std::string_view port;
  if (!authority.empty() && authority.front() == '[') {
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
  if (!port.empty()) {
    if (!port.starts_with(':')) {
      return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
    }
    const auto parsed_port = folly::tryTo<uint16_t>(port.substr(1));
    if (!parsed_port.hasValue() || parsed_port.value() == 0) {
      return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
    }
    parsed.port = parsed_port.value();
  }
  if (!uri.username().empty()) {
    parsed.username = uri.username();
  }
  if (!uri.password().empty()) {
    parsed.password = uri.password();
  }

  auto path = std::string_view(uri.path());
  if (!path.empty()) {
    if (!path.starts_with('/')) {
      return utils::Status::InvalidArgument("Redis metadata URL has invalid database path");
    }
    path.remove_prefix(1);
    if (!path.empty()) {
      const auto db = folly::tryTo<int>(path);
      if (!db.hasValue() || db.value() < 0) {
        return utils::Status::InvalidArgument("Redis metadata URL has invalid database");
      }
      parsed.db = db.value();
    }
  }

  *config = std::move(parsed);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
