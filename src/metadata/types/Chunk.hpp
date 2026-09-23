// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

/// Common head for one logical chunk. The selected overwrite mechanism owns
/// the representation behind this head; revision is a publication generation
/// used for compare-and-swap, not a physical object identifier.
struct SwordFsChunk {
  ChunkIndex index = 0;
  uint64_t start_offset = 0;
  ChunkRevision revision = kInvalidChunkRevision;
  uint64_t size = 0;

  bool operator==(const SwordFsChunk &) const = default;

  /// Return whether this descriptor matches SwordFS's fixed-size logical
  /// chunk layout for |chunk_size|. A valid descriptor establishes the
  /// logical identity and extent; it does not identify physical data.
  bool IsValidForChunkSize(uint64_t chunk_size) const;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
