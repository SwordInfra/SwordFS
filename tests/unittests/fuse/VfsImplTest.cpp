// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl basic construction and binding.
// Chunk-level I/O tests are now in chunk/ChunkManagerTest.cpp.

#include <gtest/gtest.h>

#include <memory>

#include "fuse/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::fuse::VfsImpl;

TEST(VfsImplTest, ConstructAndBind) {
  auto vfs = std::make_unique<VfsImpl>();
  auto vol = std::make_unique<swordfs::volume::VolumeImpl>();
  vfs->Bind(std::move(vol));
  // If we get here without crashing, Bind() succeeded.
  SUCCEED();
}
