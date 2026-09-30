// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/cow/COWChunkMetadata.hpp"

namespace swordfs::chunk::cow {
namespace {

TEST(COWObjectKeyTest, TypedIdentityUsesChunkIdAndCowRevisionWithoutFileCoordinates) {
  const metadata::ChunkID chunk_id(42);
  const metadata::cow::COWChunkRevision revision(7);

  const COWObjectKey key(chunk_id, revision);
  EXPECT_EQ(static_cast<std::string_view>(key), "42/7");
}

}  // namespace
}  // namespace swordfs::chunk::cow
