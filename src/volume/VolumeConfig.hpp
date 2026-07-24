// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeConfig — persistent volume metadata stored in volume.json.
//
// Created by `swordfs format` and read by `swordfs mount` to recover
// the storage backend configuration without re-specifying CLI flags.

#pragma once

#include <memory>
#include <string>

#include "utils/Status.hpp"

namespace swordfs::storage {

class IDataEngine;

/// Volume-level metadata written by `swordfs format`.
struct VolumeConfig {
  std::string name;           // volume name, set via --volume
  std::string uuid;
  std::string meta_url;       // e.g. "memory://local", "redis://..."
  std::string bucket;         // e.g. "s3://endpoint/bucket/prefix"

  /// Generate a random UUID v4 string.
  static std::string GenerateUUID();

  /// Serialize to a JSON string.
  std::string ToJson() const;

  /// Parse from a JSON string.  Returns an error Status on failure.
  static utils::Status FromJson(std::string_view json, VolumeConfig* out);

  /// Write this config to path/volume.json.  Creates parent directories.
  utils::Status WriteToFile(const std::string& path) const;

  /// Read a VolumeConfig from path/volume.json.
  static utils::Status ReadFromFile(const std::string& path, VolumeConfig* out);
};

/// Create the data engine described by a VolumeConfig.
///
/// Returns nullptr when:
/// - vol.storage is empty (memory-only, no data engine needed), or
/// - the required backend is not compiled in (e.g. ENABLE_S3=OFF).
///
/// The caller (Mount.cpp) does not need to know which backends are
/// available — the registry + conditional compilation handle that.
std::unique_ptr<IDataEngine> CreateDataEngine(const VolumeConfig& vol);

}  // namespace swordfs::storage
