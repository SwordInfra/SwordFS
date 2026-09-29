// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "FiberTest.hpp"
#include "metadata/IPrivateMetadata.hpp"
#include "metadata/mem/MemPrivateMetadataStore.hpp"

namespace swordfs::metadata {
namespace {

using COWSequenceA = PrivateSequenceTag<ChunkType::kCow, 11>;
using COWSequenceB = PrivateSequenceTag<ChunkType::kCow, 12>;
using SliceSequenceA = PrivateSequenceTag<ChunkType::kChunkSlice, 11>;

FIBER_TEST(MemPrivateMetadataStoreTest, AllocatesMonotonicMechanismScopedIndependentSequences) {
  MemPrivateMetadataStore cow(ChunkType::kCow);
  MemPrivateMetadataStore chunk_slice(ChunkType::kChunkSlice);
  uint64_t value = 0;

  EXPECT_EQ(cow.mechanism(), ChunkType::kCow);
  ASSERT_TRUE(cow.AllocateSequence(COWSequenceA{}, &value).ok());
  EXPECT_EQ(value, 1U);
  ASSERT_TRUE(cow.AllocateSequence(COWSequenceA{}, &value).ok());
  EXPECT_EQ(value, 2U);
  ASSERT_TRUE(cow.AllocateSequence(COWSequenceB{}, &value).ok());
  EXPECT_EQ(value, 1U);
  ASSERT_TRUE(chunk_slice.AllocateSequence(SliceSequenceA{}, &value).ok());
  EXPECT_EQ(value, 1U);

  EXPECT_EQ(cow.AllocateSequence(SliceSequenceA{}, &value).ToErrno(), EINVAL);
  EXPECT_EQ(cow.AllocateSequence(COWSequenceA{}, nullptr).ToErrno(), EINVAL);
}

#ifndef NDEBUG
TEST(MemPrivateMetadataStoreTest, RuntimeAllocationRejectsThreadCaller) {
  MemPrivateMetadataStore store(ChunkType::kCow);
  uint64_t value = 0;
  EXPECT_DEATH(
      { (void)store.AllocateSequence(COWSequenceA{}, &value); },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}
#endif

}  // namespace
}  // namespace swordfs::metadata
