// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS format subcommand — initialise a new volume.
//
// Creates persistent volume metadata through the selected metadata backend.
// The Redis backend stores volume configuration and the root inode atomically.

#include "cmd/Format.hpp"

#include <folly/logging/xlog.h>

#include "config/ConfigCenter.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::cmd {

int RunFormat() {
  auto &cfg = swordfs::config::ConfigCenter::Instance();

  swordfs::volume::VolumeImpl vol;
  auto status = vol.CreateFrom(cfg);
  if (!status.ok()) {
    SWORDFS_PROMPT_INFO << "Error: " << status.message();
    return 1;
  }

  SWORDFS_LOG_INFO << "Volume '" << vol.config().name << "' formatted successfully. Mount with: swordfs mount --volume "
                   << vol.config().name << " --meta " << cfg.meta_url() << " /mnt/swordfs";
  return 0;
}

}  // namespace swordfs::cmd
