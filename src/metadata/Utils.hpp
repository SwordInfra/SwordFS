// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/stat.h>

#include "utils/Status.hpp"

namespace swordfs::metadata {

// Create stat struct for ino
struct stat MakeStat(mode_t mode, time_t mtime);

// Convert st_mode to dirent type (DT_DIR, DT_REG, etc.)
uint32_t ModeToDt(mode_t mode);

// Clear the SUID/SGID bits in |st| (FUSE_CAP_HANDLE_KILLPRIV
// semantics): required when file ownership changes or the file is
// truncated/written.
void KillSUID(struct stat *st);

/// Extracts and normalizes the scheme from a URL such as "redis://host".
utils::Status ParseUrlScheme(std::string_view url, std::string *scheme);

}  // namespace swordfs::metadata
