// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "config/ConfigCenter.hpp"

#include "cmd/Format.hpp"
#include "cmd/Mount.hpp"
#include "config/Validator.hpp"
#include "metadata/Meta.hpp"
#include "storage/StorageUrl.hpp"

namespace swordfs::config {

void ConfigCenter::ConfigureOptions(CLI::App& app) {
  static const std::unordered_map<std::string, std::string> kLogLevelMap = {
      {"info", "INFO"},
      {"debug", "DBG0"},
      {"warn", "WARN"},
      {"error", "ERR"},
  };

  // Global flags and options
  app.add_flag("-f,--foreground", foreground_, "Run in foreground");
  app.add_flag_callback("-V,--version", PrintVersion, "Show version information");
  app.add_option("--log-file", log_.path, "Log file path");
  app.add_option("--log-level", log_.level, "Log level (info, debug, warn, error)")
      ->transform(CLI::CheckedTransformer(kLogLevelMap, CLI::ignore_case));
  app.add_option("--fuse-threads", fuse_threads_, "FUSE worker thread count")
      ->check(CLI::PositiveNumber)
      ->check(CLI::Range(1, static_cast<int>(std::thread::hardware_concurrency())));

  // Mount options
  RegisterMountOptions(app);

  // Format options
  RegisterFormatOptions(app);
}

void ConfigCenter::RegisterMountOptions(CLI::App& app) {
  auto cmd = app.add_subcommand("mount", "Mount a filesystem");
  cmd->add_option("mountpoint", mountpoint_, "Mount point directory (created if needed)")
      ->required();
  cmd->add_option("--volume", volume_,
                  "Volume name to mount")
      ->required();
  cmd->add_option("--meta", meta_url_,
                  "Metadata engine URL (e.g. memory://local, redis://...)")
      ->required()
      ->check(swordfs::config::ValidateMetaUrl);
  cmd->add_option("--volume-config-path", volume_config_path_,
                  "Volume config directory (required for --meta memory://local)");
  cmd->add_option("-o", fuse_opts_, "FUSE mount options (e.g. -o allow_other,ro)")
      ->allow_extra_args(false);

  cmd->parse_complete_callback([this]() {
    if (volume_config_path_.empty() ==
        swordfs::metadata::IsMemoryMode(meta_url_)) {
      throw CLI::ValidationError(
          "--volume-config-path and --meta memory://local must be used together");
    }
  });

  SubCommand sc;
  sc.cmd = cmd;
  sc.run = swordfs::cmd::RunMount;
  sub_commands_.push_back(sc);
}

void ConfigCenter::RegisterFormatOptions(CLI::App& app) {
  auto cmd = app.add_subcommand("format", "Initialise a new SwordFS volume");

  cmd->add_option("--volume", volume_, "Volume name")
      ->required();
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
  cmd->add_option("--volume-config-path", volume_config_path_,
                  "Volume config directory (required for --meta memory://local)");

  cmd->parse_complete_callback([this]() {
    if (volume_config_path_.empty() ==
        swordfs::metadata::IsMemoryMode(meta_url_)) {
      throw CLI::ValidationError(
          "--volume-config-path and --meta memory://local must be used together");
    }
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