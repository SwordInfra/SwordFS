// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
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
  // Default sparse publication keeps the existing small-interval behavior.
  // The 1MiB modes are opt-in experiments that materialize committed bytes
  // inside a dirty window and publish one contiguous logical record instead.
  enum class RmwStrategy {
    kSparse,
    kFixed1MiB,
    kSpan1MiB,
  };

  // An unpublished upload intent; keep its implementation private to the
  // translation unit while the Chunk retains stable shared ownership.
  struct FrozenSlice;

  // Each event affects only records already committed when it was captured.
  // Later records are constrained only by newer sanitation events.
  struct BoundaryClamp {
    size_t visible_prefix;
    std::optional<metadata::ChunkID> chunk_id;
    size_t record_count;
  };

  static RmwStrategy RmwStrategyFromEnvironment();

  void CaptureBoundary(size_t visible_prefix);
  void FreezeActiveLocked();
  // Consolidate ungrouped frozen intervals into one RMW candidate per touched
  // 1MiB window. Committed bytes are read outside mutex_; truncate_epoch_
  // prevents installing a candidate assembled against a since-clipped view.
  utils::Status PrepareWindowCandidates(std::vector<std::shared_ptr<FrozenSlice>> *candidates);
  // Read only committed Slice bytes; local dirty/frozen overlays are excluded.
  // Missing bytes and ranges outside the current EOF remain zero.
  utils::Status ReadCommittedRange(size_t offset, size_t len, std::string *out) const;
  utils::Status PublishFrozen(const std::shared_ptr<FrozenSlice> &candidate);
  void RebuildDirtyLocked();

  metadata::InodeID ino_;
  size_t chunk_size_;
  std::shared_ptr<metadata::slice::SliceChunkMetadata> slice_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  RmwStrategy rmw_strategy_;
  mutable utils::FiberRWMutex mutex_;
  // Non-overlapping sorted dirty intervals. Overwrites replace old portions;
  // segments outside the modified range preserve accepted local bytes.
  std::map<size_t, std::string> dirty_;
  // Unfrozen written ranges are merged only when actually contiguous.
  // dirty_ remains the sole Read-visible projection across all generations.
  std::map<size_t, std::string> active_;
  size_t active_bytes_ = 0;
  // A new Write can auto-freeze while a prior Put is in flight; stable
  // candidate ownership is independent of vector growth and retry timing.
  std::vector<std::shared_ptr<FrozenSlice>> frozen_;
  bool flushing_ = false;
  // Ordered by nondecreasing record_count for one attached ChunkID.
  // Same-history events coalesce; a new attachment resets old provenance.
  std::vector<BoundaryClamp> boundaries_;
  std::optional<utils::Status> boundary_error_;
  // Incremented by TruncateLocal so an RMW assembly can detect that its
  // source snapshot was clipped before it is installed into frozen_.
  size_t truncate_epoch_ = 0;
};

}  // namespace swordfs::chunk::slice
