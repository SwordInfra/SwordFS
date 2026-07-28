// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// CLI11 validators for ConfigCenter options.
// Kept in a separate translation unit to keep ConfigCenter.cpp lean.

#pragma once

#include <CLI/CLI.hpp>

namespace swordfs::config {

/// Validates --meta URL format and scheme (e.g. memory://local).
extern const CLI::Validator ValidateMetaUrl;

/// Validates --bucket URL has a recognised storage scheme (e.g. s3://).
extern const CLI::Validator ValidateBucketUrl;

}  // namespace swordfs::config
