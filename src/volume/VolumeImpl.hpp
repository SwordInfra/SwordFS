// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeImpl — volume lifecycle logic (format, mount).
//
// Owns a VolumeConfig and handles the higher-level operations that
// involve the ConfigCenter and storage engine initialisation.

#pragma once

#include "utils/Status.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs::config {
class ConfigCenter;
}

namespace swordfs::volume {

class VolumeImpl {
 public:
  using Status = utils::Status;

  /// Build config from CLI flags and persist to disk (format).
  Status CreateFrom(const swordfs::config::ConfigCenter& cfg);

  /// Load config from persistent store and initialise the data engine (mount).
  Status LoadFrom(const swordfs::config::ConfigCenter& cfg);

  const VolumeConfig& config() const { return config_; }

 private:
  VolumeConfig config_;
};

}  // namespace swordfs::volume
