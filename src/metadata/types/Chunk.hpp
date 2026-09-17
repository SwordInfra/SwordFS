// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

/// Metadata for one flushed chunk.
struct SwordFsChunk {
  ChunkIndex index = 0;
  uint64_t start_offset = 0;
  ChunkRevision revision = kInvalidChunkRevision;
  uint64_t size = 0;

  bool operator==(const SwordFsChunk &) const = default;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
