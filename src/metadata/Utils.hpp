// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Inode.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Convert metadata mode bits to dirent type (DT_DIR, DT_REG, etc.).
uint32_t ModeToDt(uint32_t mode);

inline constexpr uint64_t kMaxNameLength = 255;

/// Validate one directory-entry name before backend lookup or mutation.
utils::Status ValidateNameComponent(std::string_view name);

/// Validate the file type encoded in a mknod mode. Directories and symlinks
/// have dedicated namespace operations and are intentionally excluded.
utils::Status ValidateMknodMode(uint32_t mode);

/// Device identity is meaningful only for character and block devices.
uint64_t NormalizeMknodRdev(uint32_t mode, uint64_t rdev);

/// Extracts and normalizes the scheme from a URL such as "redis://host".
utils::Status ParseUrlScheme(std::string_view url, std::string *scheme);

}  // namespace swordfs::metadata
