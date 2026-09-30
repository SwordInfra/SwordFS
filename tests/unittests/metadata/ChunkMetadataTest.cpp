// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "metadata/ChunkMetadata.hpp"

namespace swordfs::metadata {
namespace {

TEST(ChunkMetadataTest, ChunkIDIsStrongOpaqueIdentityWithInvalidZero) {
  EXPECT_EQ(ChunkID{}, kInvalidChunkID);

  const ChunkID first(1);
  const ChunkID same(1);
  const ChunkID second(2);
  EXPECT_NE(first, kInvalidChunkID);
  EXPECT_EQ(first, same);
  EXPECT_NE(first, second);
}

TEST(ChunkMetadataTest, CheckedChunkIDRangeStartsAtOneAndFailsClosedAtPortableMaximum) {
  uint64_t current = 0;
  ChunkID id;
  ASSERT_TRUE(internal::AllocateChunkIDValue(&current, &id).ok());
  EXPECT_EQ(current, 1U);
  EXPECT_EQ(id, ChunkID(1));

  current = kMaxChunkIDValue;
  EXPECT_EQ(internal::AllocateChunkIDValue(&current, &id).ToErrno(), EIO);
  EXPECT_EQ(current, kMaxChunkIDValue);
  EXPECT_EQ(internal::AllocateChunkIDValue(nullptr, &id).ToErrno(), EINVAL);
  EXPECT_EQ(internal::AllocateChunkIDValue(&current, nullptr).ToErrno(), EINVAL);
}

}  // namespace
}  // namespace swordfs::metadata
