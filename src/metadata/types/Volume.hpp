// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

enum class ChunkType : uint32_t {
  kCow = 1,
  kChunkSlice = 2,
  kRedisCache = 3,
};

bool IsKnownChunkType(ChunkType chunk_type);
std::string_view ChunkTypeName(ChunkType chunk_type);
std::string ChunkTypeKey(ChunkType chunk_type);
utils::Status ParseChunkType(std::string_view name, ChunkType *out);

/// Volume-level metadata persisted by `swordfs format`.
struct SwordFsVolume {
  std::string name;
  std::string storage;
  std::string bucket;
  std::string region;
  uint64_t chunk_size = 64ULL * 1024 * 1024;
  // Chosen once at format and used as the single runtime chunk-type identity.
  ChunkType chunk_type = ChunkType::kCow;
  // Persistent filesystem semantic selected at format time, not per mount.
  bool enable_posix_acl = false;

  /// Serialize the volume metadata into its canonical binary representation.
  std::string SerializeTo() const;

  /// Parse the canonical binary volume metadata representation.
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
