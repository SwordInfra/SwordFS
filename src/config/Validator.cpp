// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "config/Validator.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "metadata/Meta.hpp"
#include "storage/StorageRegistry.hpp"

namespace swordfs::config {

const CLI::Validator ValidateMetaUrl = CLI::Validator(
    [](const std::string& input) -> std::string {
      if (input == swordfs::metadata::kMemoryMetaUrl) {
        return {};
      }
      return "Unsupported metadata engine '" + input +
             "'. Supported: memory://local";
    },
    "META_URL");

const CLI::Validator ValidateBucketUrl = CLI::Validator(
    [](const std::string& input) -> std::string {
      auto pos = input.find("://");
      if (pos == std::string::npos) {
        return "Bucket URL must have a scheme:// prefix, got: " + input;
      }
      std::string scheme = input.substr(0, pos);
      std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (swordfs::storage::StorageRegistry::Instance().Available(scheme)) {
        return {};
      }
      return "Unsupported bucket scheme '" + scheme + "', got: " + input;
    },
    "BUCKET_URL");

}  // namespace swordfs::config
