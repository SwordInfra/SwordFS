// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

inline constexpr uint64_t kMaxSupportedFileSize = static_cast<uint64_t>(std::numeric_limits<off_t>::max());

struct ChunkPosition {
  ChunkIndex index = 0;
  uint64_t start_offset = 0;
  uint64_t offset_in_chunk = 0;
};

utils::Status CalculateChunkPosition(off_t file_offset, uint64_t chunk_size, ChunkPosition *out);
utils::Status CalculateFileRangeEnd(off_t file_offset, uint64_t length, uint64_t *out);
utils::Status CalculateChunkStartOffset(ChunkIndex index, uint64_t chunk_size, uint64_t *out);

/// Common head for one logical chunk. In the current whole-object path,
/// revision is both the publication/CAS generation and the immutable physical
/// object revision. That mechanism-specific meaning is transitional under
/// #312; it is not part of the target mechanism-neutral common contract.
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
