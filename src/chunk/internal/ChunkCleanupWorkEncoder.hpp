// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <vector>

#include "metadata/ChunkSizePlan.hpp"
#include "metadata/types/Reclaim.hpp"
#include "metadata/types/Volume.hpp"

namespace swordfs::chunk::internal {

// Pure, mechanism-owned serialization of detached FileMetadata references.
// These records are only maintenance candidates; the GC participant performs
// the authoritative reachability check before any physical deletion.
class ChunkCleanupWorkEncoder {
 public:
  virtual ~ChunkCleanupWorkEncoder() = default;

  virtual utils::Status EncodeDetachedDelete(metadata::InodeID ino, const metadata::ChunkMapping &mapping,
                                             metadata::PendingDelete *out) const = 0;
  virtual utils::Status EncodeDetachedReclaim(metadata::InodeID ino,
                                              const std::vector<metadata::ChunkMapping> &mappings,
                                              metadata::ReclaimWork *out) const = 0;
};

utils::Status CreateChunkCleanupWorkEncoder(metadata::ChunkType chunk_type,
                                            std::unique_ptr<ChunkCleanupWorkEncoder> *out);

}  // namespace swordfs::chunk::internal
