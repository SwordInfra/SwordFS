// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>
#include <initializer_list>
#include <limits>
#include <vector>

#include "metadata/ChunkMetadata.hpp"
#include "metadata/ChunkSizePlan.hpp"

namespace swordfs::metadata {
namespace {

constexpr uint64_t kChunkSize = 100;

ChunkMapping Mapping(ChunkIndex index, uint64_t chunk_id) {
  return ChunkMapping{.index = index, .chunk_id = ChunkID(chunk_id)};
}

std::vector<ChunkMapping> Mappings(std::initializer_list<ChunkMapping> mappings) {
  return mappings;
}

TEST(ChunkSizePlanTest, LayoutClassifiesZeroExactAndInteriorBoundaries) {
  ChunkSizeLayout layout;

  ASSERT_TRUE(PlanChunkSizeLayout(0, kChunkSize, &layout).ok());
  EXPECT_EQ(layout.first_detached_index, 0U);
  EXPECT_FALSE(layout.boundary.has_value());
  EXPECT_TRUE(layout.ShouldDetach(0));

  ASSERT_TRUE(PlanChunkSizeLayout(100, kChunkSize, &layout).ok());
  EXPECT_EQ(layout.first_detached_index, 1U);
  EXPECT_FALSE(layout.boundary.has_value());
  EXPECT_FALSE(layout.ShouldDetach(0));
  EXPECT_TRUE(layout.ShouldDetach(1));

  ASSERT_TRUE(PlanChunkSizeLayout(150, kChunkSize, &layout).ok());
  EXPECT_EQ(layout.first_detached_index, 2U);
  ASSERT_TRUE(layout.boundary.has_value());
  EXPECT_EQ(layout.boundary->index, 1U);
  EXPECT_EQ(layout.boundary->visible_prefix, 50U);
  EXPECT_FALSE(layout.ShouldDetach(1));
  EXPECT_TRUE(layout.ShouldDetach(2));
}

TEST(ChunkSizePlanTest, LayoutRejectsInvalidInput) {
  ChunkSizeLayout layout;
  EXPECT_EQ(PlanChunkSizeLayout(1, 0, &layout).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeLayout(1, kChunkSize, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeLayout(kMaxSupportedFileSize + 1, kChunkSize, &layout).ToErrno(), EINVAL);
}

TEST(ChunkSizePlanTest, ShrinkToZeroDetachesEveryMappingWithoutBoundary) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(200, 0, kChunkSize, Mappings({Mapping(0, 1), Mapping(1, 2)}), &plan).ok());

  EXPECT_EQ(plan.target_eof, 0U);
  EXPECT_EQ(plan.expected_old_state.eof, 200U);
  EXPECT_FALSE(plan.expected_old_state.boundary.has_value());
  EXPECT_FALSE(plan.boundary.has_value());
  EXPECT_EQ(plan.detached_chunk_ids, (std::vector<ChunkID>{ChunkID(1), ChunkID(2)}));
}

TEST(ChunkSizePlanTest, ShrinkAtExactBoundaryDetachesBoundaryAndLaterMappings) {
  ChunkSizePlan plan;
  ASSERT_TRUE(
      PlanChunkSizeChange(250, 100, kChunkSize, Mappings({Mapping(2, 3), Mapping(0, 1), Mapping(1, 2)}), &plan).ok());

  ASSERT_TRUE(plan.expected_old_state.boundary.has_value());
  EXPECT_EQ(plan.expected_old_state.boundary->index, 2U);
  EXPECT_EQ(plan.expected_old_state.boundary->chunk_id, ChunkID(3));
  EXPECT_EQ(plan.expected_old_state.boundary->visible_prefix, 50U);
  EXPECT_FALSE(plan.boundary.has_value());
  EXPECT_EQ(plan.detached_chunk_ids, (std::vector<ChunkID>{ChunkID(2), ChunkID(3)}));
}

TEST(ChunkSizePlanTest, ShrinkIntoMappedBoundaryRetainsOnlyBoundaryMapping) {
  ChunkSizePlan plan;
  ASSERT_TRUE(
      PlanChunkSizeChange(250, 150, kChunkSize, Mappings({Mapping(0, 1), Mapping(1, 2), Mapping(2, 3)}), &plan).ok());

  ASSERT_TRUE(plan.boundary.has_value());
  EXPECT_EQ(plan.boundary->index, 1U);
  EXPECT_EQ(plan.boundary->chunk_id, ChunkID(2));
  EXPECT_EQ(plan.boundary->visible_prefix, 50U);
  EXPECT_EQ(plan.detached_chunk_ids, (std::vector<ChunkID>{ChunkID(3)}));
}

TEST(ChunkSizePlanTest, ShrinkIntoHoleHasNoMechanismBoundary) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(250, 150, kChunkSize, Mappings({Mapping(0, 1), Mapping(2, 3)}), &plan).ok());

  EXPECT_FALSE(plan.boundary.has_value());
  EXPECT_EQ(plan.detached_chunk_ids, (std::vector<ChunkID>{ChunkID(3)}));
}

TEST(ChunkSizePlanTest, GrowFromExactBoundaryNeedsNoSanitize) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(100, 250, kChunkSize, Mappings({Mapping(0, 1)}), &plan).ok());

  EXPECT_FALSE(plan.expected_old_state.boundary.has_value());
  EXPECT_FALSE(plan.boundary.has_value());
  EXPECT_TRUE(plan.detached_chunk_ids.empty());
}

TEST(ChunkSizePlanTest, GrowFromHoleRecordsAbsentBoundaryPreconditionWithoutMechanismScan) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(150, 250, kChunkSize, Mappings({Mapping(0, 1)}), &plan).ok());

  ASSERT_TRUE(plan.expected_old_state.boundary.has_value());
  EXPECT_EQ(plan.expected_old_state.boundary->index, 1U);
  EXPECT_FALSE(plan.expected_old_state.boundary->chunk_id.has_value());
  EXPECT_EQ(plan.expected_old_state.boundary->visible_prefix, 50U);
  EXPECT_FALSE(plan.boundary.has_value());
  EXPECT_TRUE(plan.detached_chunk_ids.empty());
}

TEST(ChunkSizePlanTest, GrowFromMappedBoundarySanitizesOnlyAttachedChunkID) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(150, 350, kChunkSize, Mappings({Mapping(0, 1), Mapping(1, 2)}), &plan).ok());

  EXPECT_EQ(plan.expected_old_state.eof, 150U);
  ASSERT_TRUE(plan.expected_old_state.boundary.has_value());
  EXPECT_EQ(plan.expected_old_state.boundary->index, 1U);
  EXPECT_EQ(plan.expected_old_state.boundary->chunk_id, ChunkID(2));
  EXPECT_EQ(plan.expected_old_state.boundary->visible_prefix, 50U);
  ASSERT_TRUE(plan.boundary.has_value());
  EXPECT_EQ(plan.boundary->index, 1U);
  EXPECT_EQ(plan.boundary->chunk_id, ChunkID(2));
  EXPECT_EQ(plan.boundary->visible_prefix, 50U);
  EXPECT_TRUE(plan.detached_chunk_ids.empty());
}

TEST(ChunkSizePlanTest, GrowPreconditionDetectsConcurrentEofOrBoundaryReplacement) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(150, 350, kChunkSize, Mappings({Mapping(0, 1), Mapping(1, 2)}), &plan).ok());

  auto changed_eof = plan.expected_old_state;
  changed_eof.eof = 151;
  EXPECT_NE(changed_eof.eof, plan.expected_old_state.eof);

  auto replaced_boundary = plan.expected_old_state;
  ASSERT_TRUE(replaced_boundary.boundary.has_value());
  replaced_boundary.boundary->chunk_id = ChunkID(3);
  ASSERT_TRUE(plan.expected_old_state.boundary.has_value());
  EXPECT_NE(replaced_boundary.boundary->chunk_id, plan.expected_old_state.boundary->chunk_id);
}

TEST(ChunkSizePlanTest, UnchangedSizeOnlyCapturesOldState) {
  ChunkSizePlan plan;
  ASSERT_TRUE(PlanChunkSizeChange(150, 150, kChunkSize, Mappings({Mapping(1, 2)}), &plan).ok());

  ASSERT_TRUE(plan.expected_old_state.boundary.has_value());
  EXPECT_EQ(plan.expected_old_state.boundary->chunk_id, ChunkID(2));
  EXPECT_FALSE(plan.boundary.has_value());
  EXPECT_TRUE(plan.detached_chunk_ids.empty());
}

TEST(ChunkSizePlanTest, RejectsMalformedPlanningInputs) {
  ChunkSizePlan plan;
  EXPECT_EQ(PlanChunkSizeChange(1, 1, 0, Mappings({}), &plan).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeChange(1, 1, kChunkSize, Mappings({}), nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeChange(kMaxSupportedFileSize + 1, 1, kChunkSize, Mappings({}), &plan).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeChange(1, kMaxSupportedFileSize + 1, kChunkSize, Mappings({}), &plan).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeChange(1, 1, kChunkSize, Mappings({Mapping(0, 0)}), &plan).ToErrno(), EINVAL);
  EXPECT_EQ(PlanChunkSizeChange(1, 1, kChunkSize, Mappings({Mapping(0, kMaxChunkIDValue + 1)}), &plan).ToErrno(),
            EINVAL);
  EXPECT_EQ(PlanChunkSizeChange(1, 1, kChunkSize, Mappings({Mapping(0, 1), Mapping(0, 2)}), &plan).ToErrno(), EIO);

  const ChunkIndex overflowing_index = kMaxSupportedFileSize / kChunkSize + 1;
  EXPECT_EQ(PlanChunkSizeChange(1, 1, kChunkSize, Mappings({Mapping(overflowing_index, 1)}), &plan).ToErrno(), EINVAL);
}

}  // namespace
}  // namespace swordfs::metadata
