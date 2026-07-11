// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS configuration center — provides a process-wide singleton for
// accessing CLI-parsed settings (log path, log level, etc.) from anywhere.

#pragma once

#include <SwordfsVersion.h>
#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <CLI/CLI.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Status.hpp"

namespace swordfs::utils {

static void PrintVersion() {
  std::cout << "SwordFS version " << SWORDFS_VERSION_MAJOR << "."
            << SWORDFS_VERSION_MINOR << "." << SWORDFS_VERSION_PATCH
            << " (libfuse " << FUSE_MAJOR_VERSION << "."
            << FUSE_MINOR_VERSION << ")"
            << "\n";
}

/// Describes one registered subcommand: its CLI::App handle and the closure
/// that executes it (called after logging is initialized).
struct SubCommand {
  CLI::App* cmd;
  std::function<int()> run;
};

/// Virtual file system backend type.
enum class VfsBackend {
  kMemory,   ///< In-memory MetaStore (default).
  kInvalid,  ///< Invalid backend type.
};

/// Logging-related configuration.
struct LogConfig {
  std::string path = "/var/log/swordfs.log";
  std::string level = "INFO";
};

/// Process-wide configuration singleton.
class ConfigCenter {
 public:
  static ConfigCenter& Instance() {
    static ConfigCenter instance;
    return instance;
  }

  /// Bind CLI options directly to ConfigCenter members.
  void ConfigureOptions(CLI::App& app);
  /// Parse the CLI options, will exit the program if parse failed
  void ParseOptions(CLI::App& app, int argc, char* argv[]);
  /// Returns the selected subcommand.
  std::optional<SubCommand> SelectedSubCommand() const;

  /// Returns the log configuration.
  LogConfig& log() { return log_; }
  /// Returns the foreground mode.
  bool foreground() const { return foreground_; }
  /// Returns the VFS backend.
  VfsBackend vfs_backend() const { return vfs_backend_; }
  /// Returns the number of FUSE worker threads.
  int fuse_threads() const { return fuse_threads_; }
  /// Returns the mount point directory.
  const std::string& mountpoint() const { return mountpoint_; }

 private:
  ConfigCenter() = default;
  ConfigCenter(const ConfigCenter&) = delete;
  ConfigCenter& operator=(const ConfigCenter&) = delete;
  /// Register mount options with the CLI::App.
  void RegisterMountOptions(CLI::App& app);

 private:
  LogConfig log_;
  // -f / --foreground: run in foreground
  bool foreground_ = false;
  // --backend: VFS backend type
  VfsBackend vfs_backend_ = VfsBackend::kMemory;
  // --fuse-threads: number of FUSE worker threads (default: single-thread)
  int fuse_threads_ = 1;
  // mount point directory (positional argument)
  std::string mountpoint_;
  // Subcommands registered with the CLI::App.
  std::vector<SubCommand> sub_commands_;
};

}  // namespace swordfs::utils
