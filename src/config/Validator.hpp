// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// CLI11 validators for ConfigCenter options.
// Kept in a separate translation unit to keep ConfigCenter.cpp lean.

#pragma once

#include <CLI/CLI.hpp>

namespace swordfs::config {

/// Validates --meta URL format and scheme (e.g. memory://local).
extern const CLI::Validator ValidateMetaUrl;

/// Validates --storage against registered StorageRegistry backends.
extern const CLI::Validator ValidateStorageBackend;

}  // namespace swordfs::config
