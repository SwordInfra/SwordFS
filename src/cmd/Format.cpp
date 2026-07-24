// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS format subcommand — initialise a new volume.
//
// Creates volume metadata (volume.json) at the given path when
// --meta memory://local is used.  For persistent metadata engines
// (e.g. Redis), volume config is stored in the engine itself.

#include "cmd/Format.hpp"

#include <algorithm>
#include <vector>

#include "storage/StorageUrl.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs::cmd {

int RunFormat() {
  auto& cfg = swordfs::utils::ConfigCenter::Instance();

  const std::string& volume_name = cfg.volume_name();
  const std::string& config_path = cfg.volume_config_path();
  const std::string& meta_url = cfg.meta_url();
  const std::string& storage = cfg.storage_backend();
  const std::string& bucket = cfg.bucket_url();

  // Parse --meta URL to determine metadata engine type
  swordfs::utils::StorageUrl meta;
  if (!swordfs::utils::StorageUrl::Parse(meta_url, &meta)) {
    SWORDFS_PROMPT_INFO << "Error: invalid --meta URL: " << meta_url;
    return 1;
  }

  // Validate supported metadata engines
  static const std::vector<std::string> kSupportedMeta = {"memory"};
  if (std::find(kSupportedMeta.begin(), kSupportedMeta.end(), meta.scheme) ==
      kSupportedMeta.end()) {
    SWORDFS_PROMPT_INFO
        << "Error: unsupported metadata engine '" << meta.scheme
        << "'. Supported: memory://local";
    return 1;
  }

  // Validate supported data storage backends
  static const std::vector<std::string> kSupportedStorage = {"s3"};
  if (!storage.empty() &&
      std::find(kSupportedStorage.begin(), kSupportedStorage.end(), storage) ==
          kSupportedStorage.end()) {
    SWORDFS_PROMPT_INFO
        << "Error: unsupported storage type '" << storage
        << "'. Supported: s3";
    return 1;
  }

  // volume-config-path is required for memory://local (no persistent store)
  if (meta.scheme == "memory" && config_path.empty()) {
    SWORDFS_PROMPT_INFO
        << "Error: volume-config-path is required when using --meta memory://local";
    return 1;
  }

  // Build volume config
  swordfs::storage::VolumeConfig vol;
  vol.name = volume_name;
  vol.uuid = swordfs::storage::VolumeConfig::GenerateUUID();
  vol.meta_url = meta_url;
  vol.bucket = bucket;

  // Write volume.json only when a config path is provided
  if (!config_path.empty()) {
    auto status = vol.WriteToFile(config_path);
    if (!status.ok()) {
      SWORDFS_PROMPT_INFO << "Error: " << status.message();
      return 1;
    }
  }

  SWORDFS_LOG_INFO << "Volume formatted successfully.";
  SWORDFS_LOG_INFO << "  Name:    " << volume_name;
  SWORDFS_LOG_INFO << "  UUID:    " << vol.uuid;
  SWORDFS_LOG_INFO << "  Meta:    " << meta_url;
  if (!storage.empty()) {
    SWORDFS_LOG_INFO << "  Storage: " << storage;
    SWORDFS_LOG_INFO << "  Bucket:  " << bucket;
  }
  if (!config_path.empty()) {
    SWORDFS_LOG_INFO << "  Config:  " << config_path;
  }

  // Build a mount hint
  std::string mount_hint = "swordfs mount --volume " + volume_name;
  mount_hint += " --meta " + meta_url;
  if (!config_path.empty()) {
    mount_hint += " --volume-config-path " + config_path;
  }
  mount_hint += " /mnt/swordfs";
  SWORDFS_LOG_INFO << "Mount with: " << mount_hint;

  return 0;
}

}  // namespace swordfs::cmd
