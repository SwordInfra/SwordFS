// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/ConfigCenter.hpp"

#include "cmd/Format.hpp"
#include "cmd/Mount.hpp"

namespace swordfs::utils {

void ConfigCenter::ConfigureOptions(CLI::App& app) {
  static const std::unordered_map<std::string, VfsBackend> kBackendMap = {
      {"memory", VfsBackend::kMemory},
  };

  static const std::unordered_map<std::string, std::string> kLogLevelMap = {
      {"info", "INFO"},
      {"debug", "DBG0"},
      {"warn", "WARN"},
      {"error", "ERR"},
  };

  // Global options
  app.add_flag("-f,--foreground", foreground_, "Run in foreground");
  app.add_option("--log-file", log_.path, "Log file path");
  app.add_option("--log-level", log_.level, "Log level (info, debug, warn, error)")
      ->transform(CLI::CheckedTransformer(kLogLevelMap, CLI::ignore_case));
  app.add_option("--backend", vfs_backend_, "VFS backend type")
      ->transform(CLI::CheckedTransformer(kBackendMap, CLI::ignore_case));
  app.add_option("--fuse-threads", fuse_threads_, "FUSE worker thread count")
      ->check(CLI::PositiveNumber)
      ->check(CLI::Range(1, static_cast<int>(std::thread::hardware_concurrency())));

  // Storage backend options
  app.add_option("--storage", storage_backend_,
                 "Storage backend (s3 or empty for memory-only)");
  app.add_option("--s3-endpoint", s3_endpoint_, "S3 endpoint URL")
      ->default_str(s3_endpoint_);
  app.add_option("--s3-region", s3_region_, "S3 region")
      ->default_str(s3_region_);
  app.add_option("--s3-bucket", s3_bucket_, "S3 bucket name");
  app.add_option("--s3-prefix", s3_prefix_, "S3 object key prefix")
      ->default_str(s3_prefix_);

  app.add_flag_callback("-V,--version", PrintVersion, "Show version information");

  // Mount options
  RegisterMountOptions(app);

  // Format options
  RegisterFormatOptions(app);
}

void ConfigCenter::ParseOptions(CLI::App& app, int argc, char* argv[]) {
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    app.exit(e);
  }
}

void ConfigCenter::RegisterMountOptions(CLI::App& app) {
  auto cmd = app.add_subcommand("mount", "Mount a filesystem");
  cmd->add_option("mountpoint", mountpoint_, "Mount point directory (created if needed)")
      ->required();
  cmd->allow_extras();  // -o allow_other,ro etc. through to FUSE

  SubCommand sc;
  sc.cmd = cmd;
  sc.run = swordfs::cmd::RunMount;
  sub_commands_.push_back(sc);
}

void ConfigCenter::RegisterFormatOptions(CLI::App& app) {
  auto cmd = app.add_subcommand("format", "Initialise a new SwordFS volume");

  cmd->add_option("volume-path", volume_path_,
                  "Volume directory (e.g. /var/lib/swordfs/myvol)")
      ->required();

  // Storage backend options — duplicated from global so they can appear
  // after the "format" subcommand keyword (CLI11 positional ordering).
  cmd->add_option("--storage", storage_backend_,
                  "Storage backend (s3 or empty for memory-only)");
  cmd->add_option("--s3-endpoint", s3_endpoint_, "S3 endpoint URL");
  cmd->add_option("--s3-region", s3_region_, "S3 region");
  cmd->add_option("--s3-bucket", s3_bucket_, "S3 bucket name");
  cmd->add_option("--s3-prefix", s3_prefix_, "S3 object key prefix");

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

}  // namespace swordfs::utils