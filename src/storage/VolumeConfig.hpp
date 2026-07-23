// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeConfig — persistent volume metadata stored in volume.json.
//
// Created by `swordfs format` and read by `swordfs mount` to recover
// the storage backend configuration without re-specifying CLI flags.
// Mirrors JuiceFS's `format` → `mount` separation.

#pragma once

#include <memory>
#include <string>

#include "utils/Status.hpp"

namespace swordfs::storage {

class IDataEngine;

/// S3-specific configuration persisted in volume.json.
struct S3VolumeConfig {
  std::string bucket;
  std::string endpoint = "https://s3.amazonaws.com";
  std::string region = "us-east-1";
  std::string prefix = "swordfs/chunks";
};

/// Volume-level metadata written by `swordfs format`.
struct VolumeConfig {
  std::string uuid;
  std::string storage;  // "s3" or empty (memory-only)
  S3VolumeConfig s3_config;

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
