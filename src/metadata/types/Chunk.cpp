// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Chunk.hpp"

#include <limits>

namespace swordfs::metadata {

utils::Status CalculateChunkPosition(off_t file_offset, uint64_t chunk_size, ChunkPosition *out) {
  if (out == nullptr || file_offset < 0 || chunk_size == 0) {
    return utils::Status::InvalidArgument("invalid file offset to chunk mapping");
  }
  const auto offset = static_cast<uint64_t>(file_offset);
  const ChunkIndex index = offset / chunk_size;
  // index is derived from this already validated offset, so the product is
  // bounded by file_offset and cannot exceed the supported off_t range.
  const uint64_t start_offset = index * chunk_size;
  *out = ChunkPosition{
      .index = index,
      .start_offset = start_offset,
      .offset_in_chunk = offset - start_offset,
  };
  return utils::Status::OK();
}

utils::Status CalculateFileRangeEnd(off_t file_offset, uint64_t length, uint64_t *out) {
  if (out == nullptr || file_offset < 0) {
    return utils::Status::InvalidArgument("invalid file range");
  }
  const auto start = static_cast<uint64_t>(file_offset);
  if (length > kMaxSupportedFileSize - start) {
    return utils::Status::InvalidArgument("file range exceeds supported size");
  }
  *out = start + length;
  return utils::Status::OK();
}

utils::Status CalculateChunkStartOffset(ChunkIndex index, uint64_t chunk_size, uint64_t *out) {
  if (out == nullptr || chunk_size == 0) {
    return utils::Status::InvalidArgument("invalid chunk layout");
  }
  if (index != 0 && chunk_size > kMaxSupportedFileSize / index) {
    return utils::Status::InvalidArgument("chunk start offset overflows");
  }
  *out = index * chunk_size;
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
