// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkCleanupParticipant.hpp"

#include <utility>

#include "chunk/cow/COWCleanup.hpp"

namespace swordfs::chunk::internal {

utils::Status CreateChunkCleanupParticipant(metadata::ChunkType chunk_type, uint64_t chunk_size,
                                            metadata::ChunkMetadataPtr chunk_metadata, metadata::IMetaEngine *meta,
                                            storage::IDataEngine *data, ChunkReachabilityProbeFn reachability_probe,
                                            std::unique_ptr<ChunkCleanupParticipant> *out) {
  if (out == nullptr || meta == nullptr || data == nullptr) {
    return utils::Status::InvalidArgument("invalid chunk cleanup participant construction");
  }
  switch (chunk_type) {
    case metadata::ChunkType::kCow:
      return cow::CreateCOWCleanupParticipant(chunk_size, std::move(chunk_metadata), meta, data,
                                              std::move(reachability_probe), out);
    default:
      return utils::Status::NotSupported("chunk cleanup mechanism is not implemented");
  }
}

}  // namespace swordfs::chunk::internal
