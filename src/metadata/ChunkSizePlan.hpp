// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Mechanism-neutral classification of a target EOF. Whole mappings at or
// beyond first_detached_index do not intersect the target file. An interior
// EOF additionally identifies the one logical boundary chunk and its visible
// prefix; exact chunk-boundary EOFs have no retained partial boundary.
struct ChunkSizeBoundary {
  ChunkIndex index = 0;
  uint64_t visible_prefix = 0;
};

struct ChunkSizeLayout {
  ChunkIndex first_detached_index = 0;
  std::optional<ChunkSizeBoundary> boundary;

  bool ShouldDetach(ChunkIndex index) const {
    return index >= first_detached_index;
  }
};

utils::Status PlanChunkSizeLayout(uint64_t target_eof, uint64_t chunk_size, ChunkSizeLayout *out);

// Final FileMetadata attachment identity. FileMetadata owns only where a
// mechanism-owned chunk state is attached; it does not interpret that state.
struct ChunkMapping {
  ChunkIndex index = 0;
  ChunkID chunk_id;
};

// Snapshot of the logical chunk containing an interior EOF. A missing
// chunk_id means that logical position is a hole. Exact chunk-boundary EOFs
// have no boundary snapshot at all.
struct ChunkBoundarySnapshot {
  ChunkIndex index = 0;
  std::optional<ChunkID> chunk_id;
  uint64_t visible_prefix = 0;
};

// Mechanism-neutral boundary intent for an attached ChunkID. The selected
// mechanism decides how to make state beyond visible_prefix unobservable.
struct RetainedChunkBoundary {
  ChunkIndex index = 0;
  ChunkID chunk_id;
  uint64_t visible_prefix = 0;
};

// FileMetadata old state that a later grow publication must revalidate after
// boundary sanitation and before making a larger EOF visible.
struct FileSizePrecondition {
  uint64_t eof = 0;
  std::optional<ChunkBoundarySnapshot> boundary;
};

struct ChunkSizePlan {
  uint64_t target_eof = 0;
  FileSizePrecondition expected_old_state;
  std::optional<RetainedChunkBoundary> boundary;
  std::vector<ChunkID> detached_chunk_ids;
};

namespace internal {

utils::Status PlanChunkSizeChangeImpl(uint64_t old_size, uint64_t new_size, uint64_t chunk_size,
                                      const std::vector<ChunkMapping> &mappings, ChunkSizePlan *out);

}  // namespace internal

// Classify a size transition from one consistent FileMetadata snapshot.
//
// Shrink:
// - mappings wholly beyond target_eof are returned as detached IDs;
// - an attached interior target boundary is returned for mechanism clamp.
//
// Grow:
// - no mapping is detached;
// - only an attached interior old-EOF boundary is returned for sanitation.
//
// In both cases expected_old_state captures the old EOF and, for an interior
// boundary, whether that logical position was a hole or mapped to one ChunkID.
//
// Keep this entry point generic over the FileMetadata snapshot container so
// the final Memory/Redis adapters can pass their natural snapshot shape
// without introducing another externally linked staging-only symbol before
// #317 activates the ChunkID mapping in production.
template <typename MappingRange>
utils::Status PlanChunkSizeChange(uint64_t old_size, uint64_t new_size, uint64_t chunk_size,
                                  const MappingRange &mappings, ChunkSizePlan *out) {
  std::vector<ChunkMapping> snapshot(mappings.begin(), mappings.end());
  return internal::PlanChunkSizeChangeImpl(old_size, new_size, chunk_size, snapshot, out);
}

}  // namespace swordfs::metadata
