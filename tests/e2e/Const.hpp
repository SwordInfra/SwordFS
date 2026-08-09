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

}  // namespace swordfs::e2e
