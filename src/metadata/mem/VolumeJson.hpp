// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>

#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata::mem {

/// Memory metadata's on-disk volume.json representation.
class VolumeJson {
 public:
  static std::string Serialize(const SwordFsVolume &volume);
  static utils::Status Parse(std::string_view json, SwordFsVolume *volume);
  static utils::Status Write(const SwordFsVolume &volume, const std::string &path);
  static utils::Status Read(const std::string &path, SwordFsVolume *volume);
  static bool Exists(const std::string &path);
};

}  // namespace swordfs::metadata::mem
