// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeConfig — persistent volume metadata stored in volume.json.
//
// Created by `swordfs format` and read by `swordfs mount` to recover
// the storage backend configuration without re-specifying CLI flags.

#pragma once

#include <string>

#include "utils/Status.hpp"

namespace swordfs::config {
class ConfigCenter;
}

namespace swordfs::volume {

using Status = utils::Status;

/// Volume-level metadata written by `swordfs format`.
struct VolumeConfig {
  std::string name;      // volume name, set via --volume
  std::string meta_url;  // e.g. "memory://local", "redis://..."
  std::string storage;   // e.g. "s3"
  std::string bucket;    // e.g. "s3://endpoint/bucket/prefix"
  std::string region;    // storage region, default "auto"

  /// Serialize to a JSON string.
  std::string ToJson() const;

  /// Parse from a JSON string into this object.
  Status FromJson(std::string_view json);

  /// Write this config to path/volume.json.  Creates parent directories.
  Status WriteToFile(const std::string& path) const;

  /// Read a VolumeConfig from path/volume.json into this object.
  Status ReadFromFile(const std::string& path);

  /// Returns a `swordfs mount` command-line hint for this volume.
  std::string MountHint() const;
};

}  // namespace swordfs::volume
