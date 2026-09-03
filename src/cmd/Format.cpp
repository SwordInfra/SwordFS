// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS format subcommand — initialise a new volume.
//
// Creates volume metadata (volume.fmt) at the given path when
// --meta memory://local is used.  For persistent metadata engines
// (e.g. Redis), volume config is stored in the engine itself.

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
