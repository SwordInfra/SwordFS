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

const CLI::Validator ValidateStorageBackend = CLI::Validator(
    [](const std::string& input) -> std::string {
      // Backend names are case-insensitive — normalise before lookup.
      std::string lower = input;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (swordfs::storage::StorageRegistry::Instance().Available(lower)) {
        return {};
      }
      return "Unknown storage backend: " + input;
    },
    "STORAGE_BACKEND");

}  // namespace swordfs::config
