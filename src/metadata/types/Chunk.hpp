// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/types.h>

#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

inline constexpr uint64_t kMaxSupportedFileSize = static_cast<uint64_t>(std::numeric_limits<off_t>::max());

// Mechanism-neutral identity for one attached chunk-state materialization.
// Zero is invalid. A detached ID is never reused or reattached; later
// rematerialization receives a fresh ID.
class ChunkID {
 public:
  constexpr ChunkID() = default;
  explicit constexpr ChunkID(uint64_t value) : value_(value) {
  }

  auto operator<=>(const ChunkID &) const = default;

  constexpr uint64_t Value() const {
    return value_;
  }

 private:
  uint64_t value_ = 0;
};

inline constexpr ChunkID kInvalidChunkID;

struct ChunkPosition {
  ChunkIndex index = 0;
  uint64_t start_offset = 0;
  uint64_t offset_in_chunk = 0;
};

utils::Status CalculateChunkPosition(off_t file_offset, uint64_t chunk_size, ChunkPosition *out);
utils::Status CalculateFileRangeEnd(off_t file_offset, uint64_t length, uint64_t *out);
utils::Status CalculateChunkStartOffset(ChunkIndex index, uint64_t chunk_size, uint64_t *out);

}  // namespace swordfs::metadata
