// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include "metadata/Types.hpp"

namespace swordfs::metadata {

// Create stat struct for ino
struct stat MakeStat(mode_t mode, time_t mtime);

// Convert st_mode to dirent type (DT_DIR, DT_REG, etc.)
uint32_t ModeToDt(mode_t mode);

}  // namespace swordfs::metadata
