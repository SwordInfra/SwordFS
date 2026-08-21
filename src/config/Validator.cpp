// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "config/Validator.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "metadata/IMetaEngine.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "storage/StorageRegistry.hpp"
#include "storage/StorageUrl.hpp"

namespace swordfs::config {

const CLI::Validator ValidateMetaUrl = CLI::Validator(
    [](const std::string &input) -> std::string {
      if (input == swordfs::metadata::kMemoryMetaUrl) {
        return {};
      }
      swordfs::metadata::RedisMetaConfig config;
      if (swordfs::metadata::IsRedisMetaUrl(input)) {
        auto status = swordfs::metadata::ParseRedisMetaUrl(input, &config);
        return status.ok() ? "" : status.message();
      }
      return "Unsupported metadata engine '" + input +
             "'. Supported: memory://local, redis://host[:port][/db]";
    },
    "META_URL");

const CLI::Validator ValidateBucketUrl = CLI::Validator(
    [](const std::string &input) -> std::string {
      auto pos = input.find("://");
      if (pos == std::string::npos) {
        return "Bucket URL must have a scheme:// prefix, got: " + input;
      }
      std::string scheme = input.substr(0, pos);
      std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (!swordfs::storage::StorageRegistry::Instance().Available(scheme)) {
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