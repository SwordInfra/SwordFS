// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "FiberTest.hpp"
#include "metadata/IPrivateMetadata.hpp"
#include "metadata/mem/MemPrivateMetadataStore.hpp"

namespace swordfs::metadata {
namespace {

using WholeSequenceA = PrivateSequenceTag<ChunkOverwriteMechanism::kWholeObject, 11>;
using WholeSequenceB = PrivateSequenceTag<ChunkOverwriteMechanism::kWholeObject, 12>;
using SliceSequenceA = PrivateSequenceTag<ChunkOverwriteMechanism::kChunkSlice, 11>;

FIBER_TEST(MemPrivateMetadataStoreTest, AllocatesMonotonicMechanismScopedIndependentSequences) {
  MemPrivateMetadataStore whole_object(ChunkOverwriteMechanism::kWholeObject);
  MemPrivateMetadataStore chunk_slice(ChunkOverwriteMechanism::kChunkSlice);
  uint64_t value = 0;

  EXPECT_EQ(whole_object.mechanism(), ChunkOverwriteMechanism::kWholeObject);
  ASSERT_TRUE(whole_object.AllocateSequence(WholeSequenceA{}, &value).ok());
  EXPECT_EQ(value, 1U);
  ASSERT_TRUE(whole_object.AllocateSequence(WholeSequenceA{}, &value).ok());
  EXPECT_EQ(value, 2U);
  ASSERT_TRUE(whole_object.AllocateSequence(WholeSequenceB{}, &value).ok());
  EXPECT_EQ(value, 1U);
  ASSERT_TRUE(chunk_slice.AllocateSequence(SliceSequenceA{}, &value).ok());
  EXPECT_EQ(value, 1U);

  EXPECT_EQ(whole_object.AllocateSequence(SliceSequenceA{}, &value).ToErrno(), EINVAL);
  EXPECT_EQ(whole_object.AllocateSequence(WholeSequenceA{}, nullptr).ToErrno(), EINVAL);
}

#ifndef NDEBUG
TEST(MemPrivateMetadataStoreTest, RuntimeAllocationRejectsThreadCaller) {
  MemPrivateMetadataStore store(ChunkOverwriteMechanism::kWholeObject);
  uint64_t value = 0;
  EXPECT_DEATH(
      { (void)store.AllocateSequence(WholeSequenceA{}, &value); },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}
#endif

}  // namespace
}  // namespace swordfs::metadata
