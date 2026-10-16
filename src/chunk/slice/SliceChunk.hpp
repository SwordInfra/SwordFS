// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chunk/Chunk.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {
class IMetaEngine;
}
namespace swordfs::storage {
class IDataEngine;
}

namespace swordfs::chunk::slice {

// Logical Slice session. Only accepted dirty intervals are held locally;
// physical data for clean reads is fetched directly from published Slices.
class SliceChunk final : public Chunk {
 public:
  SliceChunk(metadata::InodeID ino, metadata::ChunkIndex index, size_t chunk_size,
             std::shared_ptr<metadata::slice::SliceChunkMetadata> slice_metadata, metadata::IMetaEngine *meta,
             storage::IDataEngine *data);

  utils::Status Read(size_t offset, size_t len, folly::IOBuf *out) const override;
  utils::Status Write(size_t offset, const folly::IOBuf &data) override;
  utils::Status Flush() override;
  void TruncateLocal(size_t size) override;
  void ClampPublishedPrefix(size_t visible_prefix) override;
  bool HasPendingWrites() const override;

 private:
  // Each event affects only records already committed when it was captured.
  // Later records are constrained only by newer sanitation events.
  struct BoundaryClamp {
    size_t visible_prefix;
    std::optional<metadata::ChunkID> chunk_id;
    size_t record_count;
  };

  void CaptureBoundary(size_t visible_prefix);

  metadata::InodeID ino_;
  size_t chunk_size_;
  std::shared_ptr<metadata::slice::SliceChunkMetadata> slice_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  mutable utils::FiberRWMutex mutex_;
  // Non-overlapping sorted dirty intervals. Overwrites replace old portions;
  // segments outside the modified range preserve accepted local bytes.
  std::map<size_t, std::string> dirty_;
  // Ordered by nondecreasing record_count for one attached ChunkID.
  // Same-history events coalesce; a new attachment resets old provenance.
  std::vector<BoundaryClamp> boundaries_;
  std::optional<utils::Status> boundary_error_;
};

}  // namespace swordfs::chunk::slice
