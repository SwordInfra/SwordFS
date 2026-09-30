// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

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
