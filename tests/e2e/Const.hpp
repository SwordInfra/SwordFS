// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Shared constants for end-to-end tests.

#pragma once

#include <fcntl.h>
#include <sys/stat.h>

namespace swordfs::e2e {

constexpr mode_t kDefaultDirMode = 0755;
constexpr mode_t kDefaultFileMode = 0644;
constexpr int kDefaultCreateFlags = O_CREAT | O_WRONLY | O_TRUNC;

/// Suggested length for "small" test payloads (< 1 KiB).
/// Callers may pass this to Fixture::GenerateData() for compact random content.
constexpr size_t kSmallContentLen = 256;

}  // namespace swordfs::e2e
