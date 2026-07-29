// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl basic construction and binding.
// Chunk-level I/O tests are now in chunk/ChunkManagerTest.cpp.

#include <gtest/gtest.h>

#include <memory>

#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::vfs::VfsImpl;

TEST(VfsImplTest, ConstructAndBind) {
  auto vfs = std::make_unique<VfsImpl>();
  auto vol = std::make_unique<swordfs::volume::VolumeImpl>();
  vfs->Init(std::move(vol));
  // If we get here without crashing, Init() succeeded.
  SUCCEED();
}
