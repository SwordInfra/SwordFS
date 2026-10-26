// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/slice/SliceDeferredCleanup.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
#include "metadata/ChunkMetadata.hpp"
#include "metadata/ChunkSizePlan.hpp"
#include "metadata/types/Reclaim.hpp"

namespace swordfs::chunk::slice {
namespace {

bool ValidChunkID(metadata::ChunkID id) {
  return id != metadata::kInvalidChunkID && id.Value() <= metadata::kMaxChunkIDValue;
}

class DeferredSliceCleanupEncoder final : public internal::ChunkCleanupWorkEncoder {
 public:
  utils::Status EncodeDetachedDelete(metadata::InodeID ino, const metadata::ChunkMapping &mapping,
                                     metadata::PendingDelete *out) const override {
    if (out == nullptr || ino == 0 || !ValidChunkID(mapping.chunk_id)) {
      return utils::Status::InvalidArgument("invalid deferred Slice detached identity");
    }
    metadata::PendingDelete pending;
    // One ChunkID per volume: duplicate retries name the same maintenance
    // work. There is intentionally no physical delete before #273.
    pending.id = "slice:chunk:" + std::to_string(mapping.chunk_id.Value());
    pending.payload = "slice:detached:" + std::to_string(ino) + ":" + std::to_string(mapping.index) + ":" +
                      std::to_string(mapping.chunk_id.Value());
    *out = std::move(pending);
    return utils::Status::OK();
  }

  utils::Status EncodeDetachedReclaim(metadata::InodeID ino, const std::vector<metadata::ChunkMapping> &mappings,
                                      metadata::ReclaimWork *out) const override {
    if (out == nullptr || ino == 0) {
      return utils::Status::InvalidArgument("invalid deferred Slice reclaim identity");
    }
    metadata::ReclaimWork pending;
    pending.ino = ino;
    pending.payload = "slice:reclaim:" + std::to_string(ino);
    for (const auto &mapping : mappings) {
      if (!ValidChunkID(mapping.chunk_id)) {
        return utils::Status::InvalidArgument("invalid ChunkID in deferred Slice reclaim");
      }
      pending.payload += ":" + std::to_string(mapping.index) + "," + std::to_string(mapping.chunk_id.Value());
    }
    *out = std::move(pending);
    return utils::Status::OK();
  }
};

}  // namespace

utils::Status CreateDeferredSliceCleanupEncoder(std::unique_ptr<internal::ChunkCleanupWorkEncoder> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("deferred Slice cleanup encoder output is null");
  }
  *out = std::make_unique<DeferredSliceCleanupEncoder>();
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::slice
