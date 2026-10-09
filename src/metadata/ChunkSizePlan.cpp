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

utils::Status ValidateFileChunkWrite(ChunkIndex index, ChunkID chunk_id, uint64_t end, uint64_t chunk_size) {
  if (!IsValidChunkID(chunk_id) || chunk_size == 0 || end > kMaxSupportedFileSize) {
    return utils::Status::InvalidArgument("invalid typed chunk write identity or size");
  }
  uint64_t start_offset = 0;
  auto status = CalculateChunkStartOffset(index, chunk_size, &start_offset);
  if (!status.ok() || end <= start_offset || end - start_offset > chunk_size) {
    return utils::Status::InvalidArgument("typed chunk write extends outside its logical position");
  }
  return utils::Status::OK();
}

utils::Status ValidateFileSizePrecondition(const FileSizePrecondition &expected, uint64_t chunk_size) {
  ChunkSizeLayout layout;
  auto status = PlanChunkSizeLayout(expected.eof, chunk_size, &layout);
  if (!status.ok()) {
    return status;
  }
  if (layout.boundary.has_value() != expected.boundary.has_value()) {
    return utils::Status::InvalidArgument("interior EOF requires an explicit FileMetadata boundary precondition");
  }
  if (!layout.boundary.has_value()) {
    return utils::Status::OK();
  }
  const auto &boundary = *expected.boundary;
  if (!layout.boundary.has_value() || boundary.index != layout.boundary->index ||
      boundary.visible_prefix != layout.boundary->visible_prefix ||
      (boundary.chunk_id.has_value() && !IsValidChunkID(*boundary.chunk_id))) {
    return utils::Status::InvalidArgument("invalid FileMetadata EOF boundary precondition");
  }
  return utils::Status::OK();
}

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

utils::Status ValidateSizeCommitPlan(const ChunkSizePlan &requested, uint64_t actual_eof, uint64_t chunk_size,
                                     const std::vector<ChunkMapping> &mappings, bool is_shrink,
                                     ChunkSizePlan *current) {
  if (current == nullptr || (is_shrink ? requested.target_eof >= requested.expected_old_state.eof
                                       : requested.target_eof <= requested.expected_old_state.eof)) {
    return utils::Status::InvalidArgument("invalid typed file size transition");
  }
  auto status = ValidateFileSizePrecondition(requested.expected_old_state, chunk_size);
  if (!status.ok()) {
    return status;
  }
  if (actual_eof != requested.expected_old_state.eof) {
    return utils::Status::AlreadyExists("stale typed file size precondition");
  }
  status = PlanChunkSizeChange(actual_eof, requested.target_eof, chunk_size, mappings, current);
  if (!status.ok()) {
    return status;
  }
  const auto &expected = requested.expected_old_state.boundary;
  const auto &actual = current->expected_old_state.boundary;
  if (expected.has_value() != actual.has_value() ||
      (expected.has_value() && (expected->index != actual->index || expected->chunk_id != actual->chunk_id ||
                                expected->visible_prefix != actual->visible_prefix))) {
    return utils::Status::AlreadyExists("stale typed file EOF boundary attachment");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
