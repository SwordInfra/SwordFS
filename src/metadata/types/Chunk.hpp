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
struct ChunkMeta {
  ChunkIndex index;
  uint64_t start_offset;
  std::string key;
  size_t size;

  utils::Status SerializeTo(std::string *out) const;
  static utils::Status ParseFrom(std::string_view data, ChunkMeta *out);
};

}  // namespace swordfs::metadata
