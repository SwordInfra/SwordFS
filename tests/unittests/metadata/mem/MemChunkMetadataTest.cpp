// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>
#include <optional>

#include "FiberTest.hpp"
#include "metadata/mem/MemCOWChunkMetadata.hpp"

namespace swordfs::metadata {
namespace {

FIBER_TEST(MemCOWChunkMetadataTest, AllocatesMonotonicVolumeScopedChunkIDs) {
  MemCOWChunkMetadata metadata;
  ChunkID first;
  ChunkID second;

  EXPECT_EQ(metadata.Type(), ChunkType::kCow);
  ASSERT_TRUE(metadata.AllocateChunkID(&first).ok());
  ASSERT_TRUE(metadata.AllocateChunkID(&second).ok());
  EXPECT_EQ(first, ChunkID(1));
  EXPECT_EQ(second, ChunkID(2));
  EXPECT_EQ(metadata.AllocateChunkID(nullptr).ToErrno(), EINVAL);
}

FIBER_TEST(MemCOWChunkMetadataTest, IndependentVolumesStartWithIndependentIdentitySpaces) {
  MemCOWChunkMetadata first_volume;
  MemCOWChunkMetadata second_volume;
  ChunkID first;
  ChunkID second;

  ASSERT_TRUE(first_volume.AllocateChunkID(&first).ok());
  ASSERT_TRUE(second_volume.AllocateChunkID(&second).ok());
  EXPECT_EQ(first, ChunkID(1));
  EXPECT_EQ(second, ChunkID(1));
}

FIBER_TEST(MemCOWChunkMetadataTest, StoresTypedHeadsWithPerChunkRevisionAllocationAndFullHeadCas) {
  MemCOWChunkMetadata metadata;
  ChunkID first_id;
  ChunkID second_id;
  ASSERT_TRUE(metadata.AllocateChunkID(&first_id).ok());
  ASSERT_TRUE(metadata.AllocateChunkID(&second_id).ok());

  cow::COWChunkRevision first_revision;
  cow::COWChunkRevision second_revision;
  cow::COWChunkRevision other_chunk_revision;
  ASSERT_TRUE(metadata.AllocateRevision(first_id, &first_revision).ok());
  ASSERT_TRUE(metadata.AllocateRevision(first_id, &second_revision).ok());
  ASSERT_TRUE(metadata.AllocateRevision(second_id, &other_chunk_revision).ok());
  EXPECT_EQ(first_revision, cow::COWChunkRevision(1));
  EXPECT_EQ(second_revision, cow::COWChunkRevision(2));
  EXPECT_EQ(other_chunk_revision, cow::COWChunkRevision(1));

  const cow::COWChunkHead first_head{first_revision, 4096};
  ASSERT_TRUE(metadata.CompareExchangeHead(first_id, std::nullopt, first_head).ok());
  EXPECT_EQ(metadata.CompareExchangeHead(first_id, std::nullopt, first_head).ToErrno(), EEXIST);

  cow::COWChunkHead loaded;
  ASSERT_TRUE(metadata.GetHead(first_id, &loaded).ok());
  EXPECT_EQ(loaded, first_head);

  const cow::COWChunkHead stale_same_revision{first_revision, 2048};
  const cow::COWChunkHead rewritten{second_revision, 3072};
  EXPECT_EQ(metadata.CompareExchangeHead(first_id, stale_same_revision, rewritten).ToErrno(), EEXIST);
  EXPECT_EQ(metadata.CompareExchangeHead(first_id, first_head, cow::COWChunkHead{first_revision, 8192}).ToErrno(),
            EINVAL);
  ASSERT_TRUE(metadata.CompareExchangeHead(first_id, first_head, rewritten).ok());

  const cow::COWChunkHead clamped{second_revision, 1024};
  ASSERT_TRUE(metadata.CompareExchangeHead(first_id, rewritten, clamped).ok());
  ASSERT_TRUE(metadata.GetHead(first_id, &loaded).ok());
  EXPECT_EQ(loaded, clamped);
  EXPECT_EQ(
      metadata.CompareExchangeHead(first_id, rewritten, cow::COWChunkHead{cow::COWChunkRevision(3), 512}).ToErrno(),
      EEXIST);

  EXPECT_EQ(metadata.EraseHead(first_id, rewritten).ToErrno(), EEXIST);
  ASSERT_TRUE(metadata.EraseHead(first_id, clamped).ok());
  EXPECT_TRUE(metadata.GetHead(first_id, &loaded).IsNotFound());
  cow::COWChunkRevision post_erase_revision;
  ASSERT_TRUE(metadata.AllocateRevision(first_id, &post_erase_revision).ok());
  EXPECT_EQ(post_erase_revision, cow::COWChunkRevision(3));
  const cow::COWChunkHead post_erase_rewrite{post_erase_revision, 512};
  EXPECT_TRUE(metadata.CompareExchangeHead(first_id, clamped, post_erase_rewrite).IsNotFound());
  EXPECT_TRUE(metadata.EraseHead(first_id, clamped).IsNotFound());
}

FIBER_TEST(MemCOWChunkMetadataTest, ErasedChunkStateDoesNotCauseChunkIdReuse) {
  MemCOWChunkMetadata metadata;
  ChunkID first_id;
  ChunkID second_id;
  ChunkID rematerialized_id;
  ASSERT_TRUE(metadata.AllocateChunkID(&first_id).ok());
  ASSERT_TRUE(metadata.AllocateChunkID(&second_id).ok());

  cow::COWChunkRevision revision;
  ASSERT_TRUE(metadata.AllocateRevision(first_id, &revision).ok());
  const cow::COWChunkHead head{revision, 1};
  ASSERT_TRUE(metadata.CompareExchangeHead(first_id, std::nullopt, head).ok());
  ASSERT_TRUE(metadata.EraseHead(first_id, head).ok());
  cow::COWChunkRevision post_erase_revision;
  ASSERT_TRUE(metadata.AllocateRevision(first_id, &post_erase_revision).ok());

  ASSERT_TRUE(metadata.AllocateChunkID(&rematerialized_id).ok());
  EXPECT_EQ(first_id, ChunkID(1));
  EXPECT_EQ(second_id, ChunkID(2));
  EXPECT_EQ(rematerialized_id, ChunkID(3));
  EXPECT_EQ(post_erase_revision, cow::COWChunkRevision(2));
}

FIBER_TEST(MemCOWChunkMetadataTest, RejectsInvalidTypedHeadOperationsAndUnsafeSameRevisionGrowth) {
  MemCOWChunkMetadata metadata;
  ChunkID chunk_id;
  ASSERT_TRUE(metadata.AllocateChunkID(&chunk_id).ok());
  cow::COWChunkRevision revision;
  ASSERT_TRUE(metadata.AllocateRevision(chunk_id, &revision).ok());
  const cow::COWChunkHead head{revision, 1024};
  ASSERT_TRUE(metadata.CompareExchangeHead(chunk_id, std::nullopt, head).ok());

  cow::COWChunkHead loaded;
  EXPECT_EQ(metadata.AllocateRevision(kInvalidChunkID, &revision).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.AllocateRevision(ChunkID(kMaxChunkIDValue + 1), &revision).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.AllocateRevision(chunk_id, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.GetHead(kInvalidChunkID, &loaded).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.GetHead(chunk_id, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.CompareExchangeHead(kInvalidChunkID, std::nullopt, head).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.CompareExchangeHead(chunk_id, std::nullopt, cow::COWChunkHead{}).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.CompareExchangeHead(chunk_id, head, cow::COWChunkHead{cow::COWChunkRevision(0), 1}).ToErrno(),
            EINVAL);
  EXPECT_EQ(metadata
                .CompareExchangeHead(chunk_id, cow::COWChunkHead{cow::COWChunkRevision(2), 1},
                                     cow::COWChunkHead{cow::COWChunkRevision(1), 1})
                .ToErrno(),
            EINVAL);
  EXPECT_EQ(metadata.CompareExchangeHead(chunk_id, head, cow::COWChunkHead{cow::COWChunkRevision{}, 1}).ToErrno(),
            EINVAL);
  EXPECT_EQ(metadata.CompareExchangeHead(chunk_id, cow::COWChunkHead{}, head).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.CompareExchangeHead(chunk_id, head, cow::COWChunkHead{revision, head.size + 1}).ToErrno(), EINVAL);
  EXPECT_EQ(metadata
                .CompareExchangeHead(chunk_id, head,
                                     cow::COWChunkHead{cow::COWChunkRevision(cow::kMaxCOWChunkRevisionValue + 1), 1})
                .ToErrno(),
            EINVAL);
  EXPECT_EQ(metadata.EraseHead(kInvalidChunkID, head).ToErrno(), EINVAL);
  EXPECT_EQ(metadata.EraseHead(chunk_id, cow::COWChunkHead{}).ToErrno(), EINVAL);

  const cow::COWChunkHead same_head{revision, 1024};
  const cow::COWChunkHead different_revision{cow::COWChunkRevision(2), 1024};
  const cow::COWChunkHead different_size{revision, 512};
  EXPECT_EQ(head, same_head);
  EXPECT_NE(head, different_revision);
  EXPECT_NE(head, different_size);
}

#ifndef NDEBUG
TEST(MemCOWChunkMetadataTest, RuntimeAllocationRejectsThreadCaller) {
  MemCOWChunkMetadata metadata;
  ChunkID id;
  EXPECT_DEATH(
      { (void)metadata.AllocateChunkID(&id); }, "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}
#endif

}  // namespace
}  // namespace swordfs::metadata
