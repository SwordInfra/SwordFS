// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "config/ConfigCenter.hpp"

#include "cmd/Format.hpp"
#include "cmd/Mount.hpp"
#include "config/Validator.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/StorageUrl.hpp"

namespace swordfs::config {

void ConfigCenter::ConfigureOptions(CLI::App& app) {
  static const std::unordered_map<std::string, std::string> kLogLevelMap = {
      {"info", "INFO"},
      {"debug", "DBG0"},
      {"warn", "WARN"},
      {"error", "ERR"},
  };

  // Global flags and options (shared by all subcommands)
  app.add_flag_callback("-V,--version", PrintVersion, "Show version information");
  app.add_option("--log-file", log_path_, "Log file path");
  app.add_option("--log-level", log_level_, "Log level (info, debug, warn, error)")
      ->transform(CLI::CheckedTransformer(kLogLevelMap, CLI::ignore_case));

  // Mount options
  RegisterMountOptions(app);

  // Format options
  RegisterFormatOptions(app);
}

void ConfigCenter::RegisterMountOptions(CLI::App& app) {
  auto cmd = app.add_subcommand("mount", "Mount a filesystem");
  cmd->add_flag("-f,--foreground", foreground_, "Run in foreground");
  cmd->add_option("mountpoint", mountpoint_, "Mount point directory (created if needed)")
      ->required();
  cmd->add_option("--volume", volume_,
                  "Volume name to mount")
      ->required();
  cmd->add_option("--meta", meta_url_,
                  "Metadata engine URL (e.g. memory://local, redis://...)")
      ->required()
      ->check(swordfs::config::ValidateMetaUrl);
  cmd->add_option("-o", fuse_opts_, "FUSE mount options (e.g. -o allow_other,ro)")
      ->allow_extra_args(false);
  cmd->add_option("--storage-thread-count", storage_thread_count_,
                  "Storage engine thread count (default: CPU count)")
      ->check(CLI::PositiveNumber);
  cmd->add_option("--meta-thread-count", meta_thread_count_,
                  "Metadata engine thread count (default: CPU count)")
      ->check(CLI::PositiveNumber);
  cmd->add_option("--fuse-threads", fuse_threads_, "FUSE worker thread count")
      ->check(CLI::PositiveNumber)
      ->check(CLI::Range(1, static_cast<int>(std::thread::hardware_concurrency())));
  cmd->add_option("--pidfile", pidfile_, "Write daemon PID to this file");



  SubCommand sc;
  sc.cmd = cmd;
  sc.run = swordfs::cmd::RunMount;
  sub_commands_.push_back(sc);
}

void ConfigCenter::RegisterFormatOptions(CLI::App& app) {
  auto cmd = app.add_subcommand("format", "Initialise a new SwordFS volume");

  cmd->add_option("--volume", volume_, "Volume name")
      ->required()
      ->check(swordfs::config::ValidateVolumeName);
  cmd->add_option("--meta", meta_url_,
                  "Metadata engine URL (e.g. memory://local)")
      ->required()
      ->check(swordfs::config::ValidateMetaUrl);
  cmd->add_option("--bucket", bucket_url_,
                  "Bucket URL (e.g. s3://mybucket.s3.amazonaws.com/chunks)")
      ->required()
      ->check(swordfs::config::ValidateBucketUrl);
  cmd->add_option("--storage-region", storage_region_,
                  "Storage region (default: auto)");
  cmd->add_option("--chunk-size", chunk_size_,
                  "Chunk size in bytes (default: 64 MiB)")
      ->check(CLI::PositiveNumber)
      ->check(CLI::Range(4096ULL, 1024ULL * 1024 * 1024));

  cmd->parse_complete_callback([this]() {
    // Derive storage backend from --bucket scheme.
    if (!bucket_url_.empty()) {
      utils::StorageUrl url;
      if (utils::StorageUrl::Parse(bucket_url_, &url)) {
        storage_backend_ = url.scheme;
      }
    }
  });

  SubCommand sc;
  sc.cmd = cmd;
  sc.run = swordfs::cmd::RunFormat;
  sub_commands_.push_back(sc);
}

std::optional<SubCommand> ConfigCenter::SelectedSubCommand() const {
  for (const auto& cmd : sub_commands_) {
    if (cmd.cmd->parsed()) {
      return cmd;
    }
  }
  return {};
}

}  // namespace swordfs::config