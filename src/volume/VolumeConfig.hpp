// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeConfig — persistent volume metadata shared by metadata backends.
//
// Memory metadata persists this configuration in volume.json; persistent
// metadata backends store it in their own metadata store.

#pragma once

#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::config {
class ConfigCenter;
}

namespace swordfs::volume {

using Status = utils::Status;

/// Volume-level metadata persisted by `swordfs format`.
struct VolumeConfig {
  std::string name;      // volume name, set via --volume
  std::string meta_url;  // e.g. "memory://local", "redis://..."
  std::string storage;   // e.g. "s3"
  std::string bucket;    // e.g. "s3://endpoint/bucket/prefix"
  std::string region;    // storage region, default "auto"
  size_t chunk_size = 64ULL * 1024 * 1024;  // immutable after format

  /// Serialize the volume metadata into its canonical representation.
  std::string SerializeTo() const;

  /// Parse the canonical volume metadata representation.
  Status ParseFrom(std::string_view data);

  /// Serialize to a JSON string for the memory volume.json file.
  std::string ToJson() const;

  /// Parse from a JSON string used by the memory volume.json file.
  Status FromJson(std::string_view json);

  /// Write this config to path/volume.json.  Creates parent directories.
  Status WriteToFile(const std::string& path) const;

  /// Read a VolumeConfig from path/volume.json into this object.
  Status ReadFromFile(const std::string& path);

  /// Returns a `swordfs mount` command-line hint for this volume.
  std::string MountHint() const;

  /// Returns true if path/volume.json already exists on disk.
  static bool ConfigFileExists(const std::string& path);
};

}  // namespace swordfs::volume
