// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS configuration center — provides a process-wide singleton for
// accessing CLI-parsed settings (log path, log level, etc.) from anywhere.

#pragma once

#include <string>

#include "utils/Status.hpp"

namespace swordfs::utils {

/// Virtual file system backend type.
enum class VfsBackend {
  kMemory,  ///< In-memory MetaStore (default).
  kInvalid, ///< Invalid backend type.
};

inline VfsBackend VfsBackendFromString(const std::string& str) {
  if (str == "memory") {
    return VfsBackend::kMemory;
  }
  return VfsBackend::kInvalid;
}

/// Logging-related configuration.
struct LogConfig {
  std::string path = "/var/log/swordfs.log";
  std::string level = "INFO";
};

/// Process-wide configuration singleton.
class ConfigCenter {
 public:
  /// Returns the singleton instance.
  static ConfigCenter& Instance();

  /// Parse CLI flags and populate config fields.
  /// Recognised flags:
  ///   -f, --foreground      run in foreground (log to stderr)
  ///   --log-file <path>     override log file path
  ///   --log-level <lvl>     override folly log level (INFO, DBG0, WARN, etc.)
  ///   --backend <type>      VFS backend (memory, default: memory)
  ///   --fuse-threads <n>   number of FUSE worker threads (0 = single-thread)
  Status ParseFromArgs(int argc, char* argv[]);

  /// Returns the log configuration.
  LogConfig& log() { return log_; }
  /// Returns the foreground mode.
  bool foreground() { return foreground_; }
  /// Returns the VFS backend.
  VfsBackend vfs_backend() { return vfs_backend_; }
  /// Returns the number of FUSE worker threads (0 = legacy single-thread loop).
  int fuse_threads() { return fuse_threads_; }

 private:
  ConfigCenter() = default;
  ConfigCenter(const ConfigCenter&) = delete;
  ConfigCenter& operator=(const ConfigCenter&) = delete;

 private:
  LogConfig log_;
  // -f / --foreground: run in foreground
  bool foreground_ = false;
  // --backend: set vfs backend
  VfsBackend vfs_backend_ = VfsBackend::kMemory;
  // --fuse-threads: number of FUSE worker threads (default: single-thread)
  int fuse_threads_ = 1;
};

}  // namespace swordfs::utils
