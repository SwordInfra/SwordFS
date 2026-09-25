// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

enum class ChunkOverwriteMechanism : uint32_t {
  kWholeObject = 1,
  kChunkSlice = 2,
  kRedisCache = 3,
};

bool IsKnownChunkOverwriteMechanism(ChunkOverwriteMechanism mechanism);
std::string_view ChunkOverwriteMechanismName(ChunkOverwriteMechanism mechanism);
std::string ChunkOverwriteMechanismKey(ChunkOverwriteMechanism mechanism);
utils::Status ParseChunkOverwriteMechanism(std::string_view name, ChunkOverwriteMechanism *out);

/// Volume-level metadata persisted by `swordfs format`.
struct SwordFsVolume {
  std::string name;
  std::string storage;
  std::string bucket;
  std::string region;
  uint64_t chunk_size = 64ULL * 1024 * 1024;
  // Chosen once at format. This is a volume-wide index schema, not a hint
  // that may be changed independently for individual chunks.
  ChunkOverwriteMechanism chunk_overwrite_mechanism = ChunkOverwriteMechanism::kWholeObject;
  uint32_t chunk_index_format_version = 1;

  /// Serialize the volume metadata into its canonical binary representation.
  std::string SerializeTo() const;

  /// Parse the canonical binary volume metadata representation.
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
