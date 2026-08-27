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

#include "utils/Status.hpp"

namespace swordfs::config {

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
  CLI::App *cmd;
  std::function<int()> run;
};

/// Logging-related configuration.
/// Process-wide configuration singleton.
class ConfigCenter {
 public:
  static ConfigCenter &Instance() {
    static ConfigCenter instance;
    return instance;
  }

  /// Bind CLI options directly to ConfigCenter members.
  void ConfigureOptions(CLI::App &app);
  /// Returns the selected subcommand.
  std::optional<SubCommand> SelectedSubCommand() const;

  /// Returns the log file path and level (set by --log-file / --log-level).
  const std::string &log_path() const { return log_path_; }
  const std::string &log_level() const { return log_level_; }

  /// Returns the foreground mode.
  bool foreground() const { return foreground_; }
  int fuse_threads() const { return fuse_threads_; }
  int storage_thread_count() const { return storage_thread_count_; }
  int meta_thread_count() const { return meta_thread_count_; }
  /// Returns the pidfile path (mount subcommand, daemon mode).
  const std::string &pidfile() const { return pidfile_; }
  /// Returns the mount point directory.
  const std::string &mountpoint() const { return mountpoint_; }

  /// Returns the metadata engine URL (e.g. memory://local, redis://host:port).
  const std::string &meta_url() const { return meta_url_; }
  void set_meta_url(const std::string &url) { meta_url_ = url; }
  /// Returns the data storage type (e.g. "s3", empty = none).
  const std::string &storage_backend() const { return storage_backend_; }
  void set_storage_backend(const std::string &b) { storage_backend_ = b; }
  /// Returns the bucket URL (e.g. "s3://endpoint/bucket/prefix").
  const std::string &bucket_url() const { return bucket_url_; }
  void set_bucket_url(const std::string &u) { bucket_url_ = u; }
  /// Returns the storage region (e.g. "auto", "us-east-1").
  const std::string &storage_region() const { return storage_region_; }
  void set_storage_region(const std::string &r) { storage_region_ = r; }
  /// Returns the chunk size in bytes (format subcommand).
  size_t chunk_size() const { return chunk_size_; }
  /// Returns the volume name (format and mount subcommands).
  const std::string &volume() const { return volume_; }
  void set_volume(const std::string &v) { volume_ = v; }
  /// Returns the FUSE mount options string (e.g. "allow_other,ro").
  const std::string &fuse_opts() const { return fuse_opts_; }

  /// Reset all state (testing only).
  void Initialize() {
    *this = ConfigCenter{};
  }

 private:
 public:
  ConfigCenter() = default;
  ConfigCenter(ConfigCenter &&) = default;
  ConfigCenter &operator=(ConfigCenter &&) = default;

 private:
  ConfigCenter(const ConfigCenter &) = delete;
  ConfigCenter &operator=(const ConfigCenter &) = delete;
  /// Register mount options with the CLI::App.
  void RegisterMountOptions(CLI::App &app);
  /// Register format options with the CLI::App.
  void RegisterFormatOptions(CLI::App &app);

 private:
  // --log-file / --log-level
  std::string log_path_ = "/var/log/swordfs.log";
  std::string log_level_ = "INFO";
  // -f / --foreground: run in foreground
  bool foreground_ = false;
  int fuse_threads_ = 1;
  int storage_thread_count_ = static_cast<int>(std::thread::hardware_concurrency());
  int meta_thread_count_ = static_cast<int>(std::thread::hardware_concurrency());
  // mount point directory (positional argument)
  std::string mountpoint_;

  // Storage engine configuration (URL format)
  std::string meta_url_;         // --meta
  std::string storage_backend_;  // derived from --bucket scheme
  std::string bucket_url_;       // --bucket
  std::string storage_region_;   // --storage-region

  // Volume configuration (format subcommand)
  std::string volume_;                       // --volume (required for format)
  std::string fuse_opts_;                    // -o FUSE mount options (e.g. allow_other,ro)
  std::string pidfile_;                      // --pidfile (mount daemon PID file)
  size_t chunk_size_ = 64ULL * 1024 * 1024;  // --chunk-size

  // Subcommands registered with the CLI::App.
  std::vector<SubCommand> sub_commands_;
};

}  // namespace swordfs::config
