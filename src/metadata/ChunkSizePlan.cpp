// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/ChunkSizePlan.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "metadata/ChunkMetadata.hpp"

namespace swordfs::metadata {
namespace {

bool IsValidChunkID(ChunkID chunk_id) {
  return chunk_id != kInvalidChunkID && chunk_id.Value() <= kMaxChunkIDValue;
}

utils::Status NormalizeMappings(uint64_t chunk_size, const std::vector<ChunkMapping> &mappings,
                                std::vector<ChunkMapping> &sorted) {
  sorted = mappings;
  std::sort(sorted.begin(), sorted.end(),
            [](const ChunkMapping &lhs, const ChunkMapping &rhs) { return lhs.index < rhs.index; });

  for (size_t i = 0; i < sorted.size(); ++i) {
    const auto &mapping = sorted[i];
    if (!IsValidChunkID(mapping.chunk_id)) {
      return utils::Status::InvalidArgument("invalid ChunkID in FileMetadata mapping");
    }
    uint64_t start_offset = 0;
    auto status = CalculateChunkStartOffset(mapping.index, chunk_size, &start_offset);
    if (!status.ok()) {
      return status;
    }
    (void)start_offset;
    if (i > 0 && sorted[i - 1].index == mapping.index) {
      return utils::Status::Malformed("duplicate FileMetadata chunk mapping");
    }
  }
  return utils::Status::OK();
}

std::optional<ChunkID> FindChunkID(const std::vector<ChunkMapping> &mappings, ChunkIndex index) {
  const auto it =
      std::lower_bound(mappings.begin(), mappings.end(), index,
                       [](const ChunkMapping &mapping, ChunkIndex candidate) { return mapping.index < candidate; });
  if (it == mappings.end() || it->index != index) {
    return std::nullopt;
  }
  return it->chunk_id;
}

std::optional<ChunkBoundarySnapshot> MakeBoundarySnapshot(const std::optional<ChunkSizeBoundary> &boundary,
                                                          const std::vector<ChunkMapping> &mappings) {
  if (!boundary.has_value()) {
    return std::nullopt;
  }
  return ChunkBoundarySnapshot{
      .index = boundary->index,
      .chunk_id = FindChunkID(mappings, boundary->index),
      .visible_prefix = boundary->visible_prefix,
  };
}

std::optional<RetainedChunkBoundary> MakeRetainedBoundary(const std::optional<ChunkSizeBoundary> &boundary,
                                                          const std::vector<ChunkMapping> &mappings) {
  const auto snapshot = MakeBoundarySnapshot(boundary, mappings);
  if (!snapshot.has_value() || !snapshot->chunk_id.has_value()) {
    return std::nullopt;
  }
  return RetainedChunkBoundary{
      .index = snapshot->index,
      .chunk_id = *snapshot->chunk_id,
      .visible_prefix = snapshot->visible_prefix,
  };
}

}  // namespace

utils::Status PlanChunkSizeLayout(uint64_t target_eof, uint64_t chunk_size, ChunkSizeLayout *out) {
  if (out == nullptr || chunk_size == 0 || target_eof > kMaxSupportedFileSize) {
    return utils::Status::InvalidArgument("invalid chunk size layout request");
  }

  const uint64_t visible_prefix = target_eof % chunk_size;
  const ChunkIndex boundary_index = target_eof / chunk_size;
  *out = ChunkSizeLayout{
      .first_detached_index = boundary_index + (visible_prefix == 0 ? 0 : 1),
      .boundary = visible_prefix == 0 ? std::nullopt
                                      : std::optional<ChunkSizeBoundary>(ChunkSizeBoundary{
                                            .index = boundary_index, .visible_prefix = visible_prefix}),
  };
  return utils::Status::OK();
}

utils::Status internal::PlanChunkSizeChangeImpl(uint64_t old_size, uint64_t new_size, uint64_t chunk_size,
                                                const std::vector<ChunkMapping> &mappings, ChunkSizePlan *out) {
  if (out == nullptr || chunk_size == 0 || old_size > kMaxSupportedFileSize || new_size > kMaxSupportedFileSize) {
    return utils::Status::InvalidArgument("invalid file size planning request");
  }

  std::vector<ChunkMapping> sorted;
  auto status = NormalizeMappings(chunk_size, mappings, sorted);
  if (!status.ok()) {
    return status;
  }

  ChunkSizeLayout old_layout;
  status = PlanChunkSizeLayout(old_size, chunk_size, &old_layout);
  if (!status.ok()) {
    return status;
  }

  ChunkSizePlan plan{
      .target_eof = new_size,
      .expected_old_state =
          FileSizePrecondition{
              .eof = old_size,
              .boundary = MakeBoundarySnapshot(old_layout.boundary, sorted),
          },
  };

  if (new_size < old_size) {
    ChunkSizeLayout new_layout;
    status = PlanChunkSizeLayout(new_size, chunk_size, &new_layout);
    if (!status.ok()) {
      return status;
    }
    plan.boundary = MakeRetainedBoundary(new_layout.boundary, sorted);
    for (const auto &mapping : sorted) {
      if (new_layout.ShouldDetach(mapping.index)) {
        plan.detached_chunk_ids.push_back(mapping.chunk_id);
      }
    }
  } else if (new_size > old_size) {
    plan.boundary = MakeRetainedBoundary(old_layout.boundary, sorted);
  }

  *out = std::move(plan);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
