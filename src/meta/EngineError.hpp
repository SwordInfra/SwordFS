// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Engine error codes — used by all IMetadataEngine and ITransaction methods.
// These are internal error codes that the meta-operations layer maps to POSIX
// errno values at the FUSE boundary.

#pragma once

#include <string_view>

namespace swordfs::meta {

enum class EngineError {
  kOk = 0,
  kNotFound,     // Key does not exist → ENOENT
  kExists,        // Key already exists → EEXIST
  kNotDir,        // Not a directory → ENOTDIR
  kNotEmpty,      // Directory not empty → ENOTDIR
  kConflict,      // Transaction write conflict → caller retries internally
  kInternal,      // Internal engine error → EIO
};

/// Return a human-readable name for the error code.
inline constexpr std::string_view EngineErrorName(EngineError e) {
  switch (e) {
    case EngineError::kOk:        return "Ok";
    case EngineError::kNotFound:  return "NotFound";
    case EngineError::kExists:    return "Exists";
    case EngineError::kNotDir:    return "NotDir";
    case EngineError::kNotEmpty:  return "NotEmpty";
    case EngineError::kConflict:  return "Conflict";
    case EngineError::kInternal:  return "Internal";
  }
  return "Unknown";
}

}  // namespace swordfs::meta
