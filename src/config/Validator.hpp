// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// CLI11 validators for ConfigCenter options.
// Kept in a separate translation unit to keep ConfigCenter.cpp lean.

#pragma once

#include <CLI/CLI.hpp>
#include <chrono>
#include <optional>
#include <string_view>

namespace swordfs::config {

/// Validates a volume name: non-empty, ASCII alphanumeric, and starts with a letter.
extern const CLI::Validator ValidateVolumeName;

/// Validates --meta URL format and scheme (e.g. redis://localhost:6379).
extern const CLI::Validator ValidateMetaUrl;

/// Validates --bucket URL has a recognised storage scheme (e.g. s3://).
extern const CLI::Validator ValidateBucketUrl;

/// Positive, non-overflowing integer durations in ms, s or m.
std::optional<std::chrono::milliseconds> ParseLockLeaseDuration(std::string_view value);
extern const CLI::Validator ValidateLockLeaseDuration;

}  // namespace swordfs::config
