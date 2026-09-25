// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

utils::Status CalculateChunkStartOffset(ChunkIndex index, uint64_t chunk_size, uint64_t *out);

/// Common head for one logical chunk. The selected overwrite mechanism owns
/// the representation behind this head; revision is a publication generation
/// used for compare-and-swap, not a physical object identifier.
struct SwordFsChunk {
  ChunkIndex index = 0;
  ChunkRevision revision = kInvalidChunkRevision;
  uint64_t size = 0;

  bool operator==(const SwordFsChunk &) const = default;

  /// Return whether this descriptor fits SwordFS's fixed-size logical chunk
  /// layout for |chunk_size|. The file offset is derived from |index|.
  bool IsValidForChunkSize(uint64_t chunk_size) const;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
