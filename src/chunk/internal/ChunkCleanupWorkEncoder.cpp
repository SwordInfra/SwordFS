// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"

#include "chunk/slice/SliceDeferredCleanup.hpp"

namespace swordfs::chunk::internal {
utils::Status CreateChunkCleanupWorkEncoder(metadata::ChunkType chunk_type,
                                            std::unique_ptr<ChunkCleanupWorkEncoder> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("cleanup work encoder output is null");
  }
  if (chunk_type == metadata::ChunkType::kChunkSlice) {
    return slice::CreateDeferredSliceCleanupEncoder(out);
  }
  return utils::Status::NotSupported("unsupported cleanup work mechanism");
}

}  // namespace swordfs::chunk::internal
