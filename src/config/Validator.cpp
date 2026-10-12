// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "config/Validator.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "storage/DataEngineRegistry.hpp"
#include "storage/StorageUrl.hpp"

namespace swordfs::config {

std::optional<std::chrono::milliseconds> ParseLockLeaseDuration(std::string_view value) {
  uint64_t scale = 0;
  if (value.ends_with("ms")) {
    scale = 1;
    value.remove_suffix(2);
  } else if (value.ends_with('s')) {
    scale = 1000;
    value.remove_suffix(1);
  } else if (value.ends_with('m')) {
    scale = 60000;
    value.remove_suffix(1);
  } else {
    return std::nullopt;
  }
  uint64_t number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size() || number == 0 ||
      number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / scale) {
    return std::nullopt;
  }
  return std::chrono::milliseconds(number * scale);
}

const CLI::Validator ValidateLockLeaseDuration = CLI::Validator(
    [](const std::string &value) -> std::string {
      return ParseLockLeaseDuration(value).has_value() ? ""
                                                       : "Expected a positive integer duration with ms, s or m suffix";
    },
    "LEASE_DURATION");

const CLI::Validator ValidateVolumeName = CLI::Validator(
    [](const std::string &input) -> std::string {
      if (input.empty()) {
        return "Volume name must not be empty";
      }
      const auto is_ascii_letter = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
      if (!is_ascii_letter(input.front())) {
        return "Volume name must start with an ASCII letter: " + input;
      }
      if (!std::all_of(input.begin(), input.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
          })) {
        return "Volume name must contain only ASCII letters and digits: " + input;
      }
      return {};
    },
    "VOLUME_NAME");

const CLI::Validator ValidateMetaUrl = CLI::Validator(
    [](const std::string &input) -> std::string {
      std::string scheme;
      auto status = swordfs::metadata::ParseUrlScheme(input, &scheme);
      if (!status.ok()) {
        return "Unsupported metadata engine '" + input + "'";
      }
      if (!swordfs::metadata::MetaEngineRegistry::Instance().Available(scheme)) {
        return "Unsupported metadata engine '" + scheme + "'";
      }
      if (scheme == "redis") {
        swordfs::metadata::RedisMetaConfig config;
        status = swordfs::metadata::ParseRedisMetaUrl(input, &config);
        return status.ok() ? "" : status.message();
      }
      return {};
    },
    "META_URL");

const CLI::Validator ValidateBucketUrl = CLI::Validator(
    [](const std::string &input) -> std::string {
      auto pos = input.find("://");
      if (pos == std::string::npos) {
        return "Bucket URL must have a scheme:// prefix, got: " + input;
      }
      std::string scheme = input.substr(0, pos);
      std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
      if (!swordfs::storage::DataEngineRegistry::Instance().Available(scheme)) {
        return "Unsupported bucket scheme '" + scheme + "', got: " + input;
      }

      // Require at least a bucket name in the path:
      //   s3://<endpoint>/<bucket>[/<prefix>]
      swordfs::utils::StorageUrl url;
      if (!swordfs::utils::StorageUrl::Parse(input, &url)) {
        return "Invalid bucket URL: " + input;
      }
      if (url.path.empty() || url.path == "/") {
        return "Bucket URL is missing bucket name. "
               "Expected format: s3://<endpoint>/<bucket>, "
               "got: " +
               input;
      }
      return {};
    },
    "BUCKET_URL");

}  // namespace swordfs::config
