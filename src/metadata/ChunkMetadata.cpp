// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/ChunkMetadata.hpp"

namespace swordfs::metadata::internal {

utils::Status AllocateChunkIDValue(uint64_t *current, ChunkID *out) {
  if (current == nullptr || out == nullptr) {
    return utils::Status::InvalidArgument("ChunkID state or output is null");
  }
  if (*current >= kMaxChunkIDValue) {
    return utils::Status::IOError("ChunkID allocator exhausted");
  }
  ++*current;
  *out = ChunkID(*current);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata::internal
