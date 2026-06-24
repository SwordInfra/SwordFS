// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS configuration center — provides a process-wide singleton for
// accessing CLI-parsed settings (log path, log level, etc.) from anywhere.

#pragma once

#include <string>

namespace swordfs::config {

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
  void ParseFromArgs(int argc, char* argv[]);

  /// Returns the log configuration.
  LogConfig& log() { return log_; }

  /// Returns the foreground mode.
  bool foreground() { return foreground_; }

 private:
  ConfigCenter() = default;
  ConfigCenter(const ConfigCenter&) = delete;
  ConfigCenter& operator=(const ConfigCenter&) = delete;

 private:
  LogConfig log_;
  bool foreground_ = false;  // -f / --foreground: run in foreground
};

}  // namespace swordfs::config
