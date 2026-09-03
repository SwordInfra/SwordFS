// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

/// Volume-level metadata persisted by `swordfs format`.
struct SwordFsVolume {
  std::string name;
  std::string storage;
  std::string bucket;
  std::string region;
  uint64_t chunk_size = 64ULL * 1024 * 1024;

  /// Serialize the volume metadata into its canonical binary representation.
  std::string SerializeTo() const;

  /// Parse the canonical binary volume metadata representation.
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
