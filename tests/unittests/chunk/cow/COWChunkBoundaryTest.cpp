// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>
#include <optional>

#include "chunk/cow/COWChunkBoundary.hpp"

namespace swordfs::chunk::cow {
namespace {

using metadata::ChunkID;
using metadata::ChunkIndex;
using metadata::RetainedChunkBoundary;
using metadata::cow::COWChunkHead;
using metadata::cow::COWChunkMetadata;
using metadata::cow::COWChunkRevision;

class ScriptedCOWChunkMetadata final : public COWChunkMetadata {
 public:
  utils::Status AllocateChunkID(ChunkID *) override {
    return utils::Status::NotSupported("not used");
  }

  utils::Status AllocateRevision(ChunkID, COWChunkRevision *) override {
    return utils::Status::NotSupported("not used");
  }

  utils::Status GetHead(ChunkID, COWChunkHead *out) override {
    ++get_calls;
    if (!get_status.ok()) {
      return get_status;
    }
    if (!head.has_value()) {
      return utils::Status::NotFound("missing");
    }
    *out = *head;
    return utils::Status::OK();
  }

  utils::Status CompareExchangeHead(ChunkID, const std::optional<COWChunkHead> &expected,
                                    const COWChunkHead &replacement) override {
    ++cas_calls;
    if (inject_conflict.has_value()) {
      head = *inject_conflict;
      inject_conflict.reset();
      return utils::Status::AlreadyExists("concurrent rewrite");
    }
    if (remove_on_cas) {
      remove_on_cas = false;
      head.reset();
      return utils::Status::NotFound("concurrent detach");
    }
    if (!cas_status.ok()) {
      return cas_status;
    }
    if (!head.has_value()) {
      return utils::Status::NotFound("missing");
    }
    if (!expected.has_value() || *expected != *head) {
      return utils::Status::AlreadyExists("changed");
    }
    head = replacement;
    return utils::Status::OK();
  }

  utils::Status EraseHead(ChunkID, const COWChunkHead &) override {
    return utils::Status::NotSupported("not used");
  }

  std::optional<COWChunkHead> head;
  std::optional<COWChunkHead> inject_conflict;
  utils::Status get_status = utils::Status::OK();
  utils::Status cas_status = utils::Status::OK();
  bool remove_on_cas = false;
  int get_calls = 0;
  int cas_calls = 0;
};

RetainedChunkBoundary Boundary() {
  return RetainedChunkBoundary{.index = 3, .chunk_id = ChunkID(9), .visible_prefix = 64};
}

TEST(COWChunkBoundaryTest, AlreadySafeHeadNeedsNoMutationOrMappingProbe) {
  ScriptedCOWChunkMetadata metadata;
  metadata.head = COWChunkHead{.revision = COWChunkRevision(7), .size = 32};
  int probe_calls = 0;

  ASSERT_TRUE(SanitizeCOWBoundary(metadata, Boundary(), [&](ChunkIndex, std::optional<ChunkID> *) {
                ++probe_calls;
                return utils::Status::OK();
              }).ok());
  EXPECT_EQ(metadata.get_calls, 1);
  EXPECT_EQ(metadata.cas_calls, 0);
  EXPECT_EQ(probe_calls, 0);
}

TEST(COWChunkBoundaryTest, ClampKeepsChunkIDAndRevisionWhileShrinkingSize) {
  ScriptedCOWChunkMetadata metadata;
  metadata.head = COWChunkHead{.revision = COWChunkRevision(7), .size = 96};

  ASSERT_TRUE(SanitizeCOWBoundary(metadata, Boundary(), [](ChunkIndex, std::optional<ChunkID> *) {
                return utils::Status::OK();
              }).ok());
  ASSERT_TRUE(metadata.head.has_value());
  EXPECT_EQ(*metadata.head, (COWChunkHead{.revision = COWChunkRevision(7), .size = 64}));
  EXPECT_EQ(metadata.cas_calls, 1);
}

TEST(COWChunkBoundaryTest, CASConflictRefreshesAndClampsNewlyObservedRevision) {
  ScriptedCOWChunkMetadata metadata;
  metadata.head = COWChunkHead{.revision = COWChunkRevision(7), .size = 96};
  metadata.inject_conflict = COWChunkHead{.revision = COWChunkRevision(8), .size = 88};

  ASSERT_TRUE(SanitizeCOWBoundary(metadata, Boundary(), [](ChunkIndex, std::optional<ChunkID> *) {
                return utils::Status::OK();
              }).ok());
  ASSERT_TRUE(metadata.head.has_value());
  EXPECT_EQ(*metadata.head, (COWChunkHead{.revision = COWChunkRevision(8), .size = 64}));
  EXPECT_EQ(metadata.get_calls, 2);
  EXPECT_EQ(metadata.cas_calls, 2);
}

TEST(COWChunkBoundaryTest, MissingHeadStillMappedToSameChunkIDFailsClosed) {
  ScriptedCOWChunkMetadata metadata;
  const auto status = SanitizeCOWBoundary(metadata, Boundary(), [](ChunkIndex index, std::optional<ChunkID> *attached) {
    EXPECT_EQ(index, 3U);
    *attached = ChunkID(9);
    return utils::Status::OK();
  });

  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(status.message(), "FileMetadata references a missing COW chunk head");
}

TEST(COWChunkBoundaryTest, MissingHeadAfterMappingReplacementStopsOnOldChunkID) {
  ScriptedCOWChunkMetadata metadata;
  ASSERT_TRUE(SanitizeCOWBoundary(metadata, Boundary(), [](ChunkIndex, std::optional<ChunkID> *attached) {
                *attached = ChunkID(10);
                return utils::Status::OK();
              }).ok());
}

TEST(COWChunkBoundaryTest, CASLosingToConcurrentDetachRevalidatesFileMapping) {
  ScriptedCOWChunkMetadata metadata;
  metadata.head = COWChunkHead{.revision = COWChunkRevision(7), .size = 96};
  metadata.remove_on_cas = true;
  int probe_calls = 0;

  ASSERT_TRUE(SanitizeCOWBoundary(metadata, Boundary(), [&](ChunkIndex, std::optional<ChunkID> *attached) {
                ++probe_calls;
                attached->reset();
                return utils::Status::OK();
              }).ok());
  EXPECT_EQ(metadata.get_calls, 2);
  EXPECT_EQ(metadata.cas_calls, 1);
  EXPECT_EQ(probe_calls, 1);
}

TEST(COWChunkBoundaryTest, PropagatesMetadataAndMappingProbeFailures) {
  ScriptedCOWChunkMetadata metadata;
  metadata.get_status = utils::Status::Unavailable("read failed");
  EXPECT_EQ(SanitizeCOWBoundary(metadata, Boundary(),
                                [](ChunkIndex, std::optional<ChunkID> *) { return utils::Status::OK(); })
                .ToErrno(),
            EIO);

  metadata.get_status = utils::Status::OK();
  EXPECT_EQ(SanitizeCOWBoundary(
                metadata, Boundary(),
                [](ChunkIndex, std::optional<ChunkID> *) { return utils::Status::Unavailable("probe failed"); })
                .ToErrno(),
            EIO);
}

TEST(COWChunkBoundaryTest, PropagatesNonConflictCASFailureAndRejectsInvalidIntent) {
  ScriptedCOWChunkMetadata metadata;
  metadata.head = COWChunkHead{.revision = COWChunkRevision(7), .size = 96};
  metadata.cas_status = utils::Status::OutcomeUnknown("CAS ambiguous");
  EXPECT_EQ(SanitizeCOWBoundary(metadata, Boundary(),
                                [](ChunkIndex, std::optional<ChunkID> *) { return utils::Status::OK(); })
                .ToErrno(),
            EIO);

  auto invalid = Boundary();
  invalid.chunk_id = swordfs::metadata::kInvalidChunkID;
  EXPECT_EQ(
      SanitizeCOWBoundary(metadata, invalid, [](ChunkIndex, std::optional<ChunkID> *) { return utils::Status::OK(); })
          .ToErrno(),
      EINVAL);
  invalid = Boundary();
  invalid.visible_prefix = 0;
  EXPECT_EQ(
      SanitizeCOWBoundary(metadata, invalid, [](ChunkIndex, std::optional<ChunkID> *) { return utils::Status::OK(); })
          .ToErrno(),
      EINVAL);
  EXPECT_EQ(SanitizeCOWBoundary(metadata, Boundary(), ChunkAttachmentProbeFn{}).ToErrno(), EINVAL);
}

}  // namespace
}  // namespace swordfs::chunk::cow
