// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata::mem {

/// Persistent storage for the memory metadata volume configuration.
class VolumeFile {
 public:
  explicit VolumeFile(std::string volume_name) : volume_name_(std::move(volume_name)) {}

  utils::Status Write(const SwordFsVolume &volume);
  utils::Status Read(SwordFsVolume *volume);
  bool Exists() const;

 private:
  std::string volume_name_;
};

}  // namespace swordfs::metadata::mem
