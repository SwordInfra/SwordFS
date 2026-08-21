// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaConfig.hpp"

#include <charconv>
#include <climits>

#include <folly/Uri.h>

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
  const auto authority_begin = meta_url.substr(kRedisScheme.size());
  const auto authority_end = authority_begin.find_first_of("/?#");
  const auto authority = authority_begin.substr(0, authority_end);
  if (authority.ends_with(":0")) {
    return utils::Status::InvalidArgument("Redis metadata URL has invalid port");
  }
  if (uri.port() != 0) {
    parsed.port = uri.port();
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
