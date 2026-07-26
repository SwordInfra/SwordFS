// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS format subcommand — initialise a new volume.
//
// Creates volume metadata (volume.json) at the given path when
// --meta memory://local is used.  For persistent metadata engines
// (e.g. Redis), volume config is stored in the engine itself.

#include "cmd/Format.hpp"

#include "config/ConfigCenter.hpp"
#include "metadata/Meta.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs::cmd {

int FormatMemoryVolume(swordfs::storage::VolumeConfig& vol);

int RunFormat() {
  auto& cfg = swordfs::config::ConfigCenter::Instance();

  // Build volume config
  swordfs::storage::VolumeConfig vol;
  vol.name = cfg.volume();
  vol.uuid = swordfs::storage::VolumeConfig::GenerateUUID();
  vol.meta_url = cfg.meta_url();
  vol.storage = cfg.storage_backend();
  vol.bucket = cfg.bucket_url();

  if (metadata::IsMemoryMode(vol.meta_url)) {
    return FormatMemoryVolume(vol);
  } else {
    SWORDFS_PROMPT_EXIT << "Unsupported metadata engine: " << vol.meta_url;
    return 1;
  }

  // Build a mount hint
  SWORDFS_LOG_INFO << "Mount with: " << vol.MountHint();

  return 0;
}

int FormatMemoryVolume(swordfs::storage::VolumeConfig& vol) {
  auto& cfg = swordfs::config::ConfigCenter::Instance();

  // Write volume.json only when a config path is provided
  if (cfg.volume_config_path().empty()) {
    SWORDFS_LOG_INFO << "Volume config path not provided; skipping volume.json creation";
    return 1;
  }

  auto status = vol.WriteToFile(cfg.volume_config_path());
  if (!status.ok()) {
    SWORDFS_PROMPT_INFO << "Error: " << status.message();
    return 1;
  }

  SWORDFS_LOG_INFO << "Volume formatted successfully.\n"
                   << vol.DebugString();
  return 0;
}

}  // namespace swordfs::cmd
