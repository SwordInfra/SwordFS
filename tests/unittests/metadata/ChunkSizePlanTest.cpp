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

TEST(ChunkSizePlanTest, TypedWriteRejectsCrossChunkAndInvalidPhysicalIdentities) {
  EXPECT_TRUE(ValidateFileChunkWrite(0, ChunkID(1), 1, kChunkSize).ok());
  EXPECT_TRUE(ValidateFileChunkWrite(3, ChunkID(2), 400, kChunkSize).ok());
  EXPECT_EQ(ValidateFileChunkWrite(0, kInvalidChunkID, 1, kChunkSize).ToErrno(), EINVAL);
  EXPECT_EQ(ValidateFileChunkWrite(0, ChunkID(kMaxChunkIDValue + 1), 1, kChunkSize).ToErrno(), EINVAL);
  EXPECT_EQ(ValidateFileChunkWrite(0, ChunkID(1), 1, 0).ToErrno(), EINVAL);
  EXPECT_EQ(ValidateFileChunkWrite(0, ChunkID(1), kChunkSize + 1, kChunkSize).ToErrno(), EINVAL);
  EXPECT_EQ(ValidateFileChunkWrite(3, ChunkID(1), 300, kChunkSize).ToErrno(), EINVAL);
  EXPECT_EQ(ValidateFileChunkWrite(3, ChunkID(1), 401, kChunkSize).ToErrno(), EINVAL);
  EXPECT_EQ(
      ValidateFileChunkWrite(kMaxSupportedFileSize / kChunkSize + 1, ChunkID(1), kMaxSupportedFileSize, kChunkSize)
          .ToErrno(),
      EINVAL);
}

TEST(ChunkSizePlanTest, TypedEofPreconditionRejectsAbsentOrWrongBoundaryGeometry) {
  EXPECT_TRUE(ValidateFileSizePrecondition({.eof = 100}, kChunkSize).ok());
  EXPECT_EQ(ValidateFileSizePrecondition({.eof = 150}, kChunkSize).ToErrno(), EINVAL);
  const FileSizePrecondition hole{
      .eof = 150, .boundary = ChunkBoundarySnapshot{.index = 1, .chunk_id = std::nullopt, .visible_prefix = 50}};
  EXPECT_TRUE(ValidateFileSizePrecondition(hole, kChunkSize).ok());
  EXPECT_EQ(ValidateFileSizePrecondition({.eof = 100, .boundary = hole.boundary}, kChunkSize).ToErrno(), EINVAL);
  auto wrong_index = hole;
  wrong_index.boundary->index = 0;
  EXPECT_EQ(ValidateFileSizePrecondition(wrong_index, kChunkSize).ToErrno(), EINVAL);
  auto wrong_prefix = hole;
  wrong_prefix.boundary->visible_prefix = 49;
  EXPECT_EQ(ValidateFileSizePrecondition(wrong_prefix, kChunkSize).ToErrno(), EINVAL);
  auto invalid_id = hole;
  invalid_id.boundary->chunk_id = ChunkID(kMaxChunkIDValue + 1);
  EXPECT_EQ(ValidateFileSizePrecondition(invalid_id, kChunkSize).ToErrno(), EINVAL);
  EXPECT_EQ(ValidateFileSizePrecondition(hole, 0).ToErrno(), EINVAL);
}

TEST(ChunkSizePlanTest, TypedSizeCommitRejectsStaleEofAndBoundaryIdentity) {
  const auto mappings = Mappings({Mapping(0, 10), Mapping(1, 11), Mapping(2, 12)});
  ChunkSizePlan shrink;
  ASSERT_TRUE(PlanChunkSizeChange(250, 125, kChunkSize, mappings, &shrink).ok());

  ChunkSizePlan authoritative;
  ASSERT_TRUE(ValidateSizeCommitPlan(shrink, 250, kChunkSize, mappings, true, &authoritative).ok());
  EXPECT_EQ(authoritative.boundary->chunk_id, ChunkID(11));
  EXPECT_EQ(authoritative.detached_chunk_ids, (std::vector<ChunkID>{ChunkID(12)}));
  EXPECT_EQ(ValidateSizeCommitPlan(shrink, 251, kChunkSize, mappings, true, &authoritative).ToErrno(), EEXIST);
  EXPECT_EQ(ValidateSizeCommitPlan(shrink, 250, kChunkSize, Mappings({Mapping(0, 10), Mapping(1, 11), Mapping(2, 13)}),
                                   true, &authoritative)
                .ToErrno(),
            EEXIST)
      << "even a soon-detached ChunkID may be the old EOF boundary precondition";

  ChunkSizePlan exact_eof_shrink;
  ASSERT_TRUE(PlanChunkSizeChange(300, 125, kChunkSize, mappings, &exact_eof_shrink).ok());
  EXPECT_TRUE(ValidateSizeCommitPlan(exact_eof_shrink, 300, kChunkSize,
                                     Mappings({Mapping(0, 10), Mapping(1, 11), Mapping(2, 13)}), true, &authoritative)
                  .ok())
      << "a detached ID outside an exact old-EOF boundary must be classified afresh";
  EXPECT_EQ(authoritative.detached_chunk_ids, (std::vector<ChunkID>{ChunkID(13)}));
  EXPECT_EQ(ValidateSizeCommitPlan(shrink, 250, kChunkSize, Mappings({Mapping(0, 10), Mapping(1, 11), Mapping(2, 13)}),
                                   true, nullptr)
                .ToErrno(),
            EINVAL);

  auto stale_boundary = shrink;
  stale_boundary.expected_old_state.boundary->chunk_id = ChunkID(13);
  EXPECT_EQ(ValidateSizeCommitPlan(stale_boundary, 250, kChunkSize, mappings, true, &authoritative).ToErrno(), EEXIST);
  auto missing_boundary = shrink;
  missing_boundary.expected_old_state.boundary.reset();
  EXPECT_EQ(ValidateSizeCommitPlan(missing_boundary, 250, kChunkSize, mappings, true, &authoritative).ToErrno(),
            EINVAL);
  EXPECT_EQ(ValidateSizeCommitPlan(shrink, 250, kChunkSize, mappings, false, &authoritative).ToErrno(), EINVAL);

  ChunkSizePlan grow;
  ASSERT_TRUE(PlanChunkSizeChange(125, 250, kChunkSize, Mappings({Mapping(1, 11)}), &grow).ok());
  ASSERT_TRUE(ValidateSizeCommitPlan(grow, 125, kChunkSize, Mappings({Mapping(1, 11)}), false, &authoritative).ok());
  EXPECT_EQ(ValidateSizeCommitPlan(grow, 125, kChunkSize, Mappings({Mapping(1, 12)}), false, &authoritative).ToErrno(),
            EEXIST);
  EXPECT_EQ(ValidateSizeCommitPlan(grow, 125, kChunkSize, Mappings({Mapping(1, 11)}), true, &authoritative).ToErrno(),
            EINVAL);
}

}  // namespace
}  // namespace swordfs::metadata
