// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS format subcommand — initialise a new volume.
//
// Mirrors JuiceFS `format`: creates volume metadata (volume.json) at the
// given path.  The config is then read by `swordfs mount` to recover the
// storage backend without re-specifying CLI flags.

#include "cmd/Format.hpp"

#include "storage/VolumeConfig.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Logging.hpp"

namespace swordfs::cmd {

int RunFormat() {
  auto& cfg = swordfs::utils::ConfigCenter::Instance();

  const std::string& volume_path = cfg.volume_path();
  const std::string& storage = cfg.storage_backend();

  swordfs::storage::VolumeConfig vol;
  vol.uuid = swordfs::storage::VolumeConfig::GenerateUUID();
  vol.storage = storage;

  if (storage == "s3") {
    if (cfg.s3_bucket().empty()) {
      SWORDFS_PROMPT_INFO
          << "Error: --s3-bucket is required when --storage=s3";
      return 1;
    }
    vol.s3_config.bucket = cfg.s3_bucket();
    vol.s3_config.endpoint = cfg.s3_endpoint();
    vol.s3_config.region = cfg.s3_region();
    vol.s3_config.prefix = cfg.s3_prefix();
  }

  auto status = vol.WriteToFile(volume_path);
  if (!status.ok()) {
    SWORDFS_PROMPT_INFO << "Error: " << status.message();
    return 1;
  }

  std::cout << "Volume formatted successfully.\n"
            << "  Path:    " << volume_path << "\n"
            << "  UUID:    " << vol.uuid << "\n"
            << "  Storage: " << (storage.empty() ? "memory" : storage) << "\n";
  if (storage == "s3") {
    std::cout << "  S3 Bucket:   " << vol.s3_config.bucket << "\n"
              << "  S3 Endpoint: " << vol.s3_config.endpoint << "\n"
              << "  S3 Region:   " << vol.s3_config.region << "\n";
  }
  std::cout << "\nMount with:\n"
            << "  swordfs mount --volume " << volume_path << " /mnt/swordfs\n";

  return 0;
}

}  // namespace swordfs::cmd
