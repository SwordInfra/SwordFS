// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"

#include "chunk/cow/COWCleanup.hpp"

namespace swordfs::chunk::internal {
namespace {

class COWCleanupWorkEncoder final : public ChunkCleanupWorkEncoder {
 public:
  utils::Status EncodeDetachedDelete(metadata::InodeID ino, const metadata::ChunkMapping &mapping,
                                     metadata::PendingDelete *out) const override {
    return cow::FreezeCOWDetachedDelete(ino, mapping, out);
  }

  utils::Status EncodeDetachedReclaim(metadata::InodeID ino, const std::vector<metadata::ChunkMapping> &mappings,
                                      metadata::ReclaimWork *out) const override {
    return cow::FreezeCOWDetachedReclaim(ino, mappings, out);
  }
};

}  // namespace

utils::Status CreateChunkCleanupWorkEncoder(metadata::ChunkType chunk_type,
                                            std::unique_ptr<ChunkCleanupWorkEncoder> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("cleanup work encoder output is null");
  }
  if (chunk_type != metadata::ChunkType::kCow) {
    return utils::Status::NotSupported("unsupported cleanup work mechanism");
  }
  *out = std::make_unique<COWCleanupWorkEncoder>();
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::internal
