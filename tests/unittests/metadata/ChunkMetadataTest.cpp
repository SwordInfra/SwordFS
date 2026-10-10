// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace swordfs::metadata
