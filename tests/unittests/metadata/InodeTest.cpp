// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/types/Inode.hpp"

using swordfs::metadata::SwordFsAttr;

namespace {

TEST(SwordFsAttrTest, RegularFilePosixBlocksUseStoredValueOrNonzeroFallback) {
  struct Case {
    uint64_t size;
    uint64_t stored_blocks;
    blkcnt_t expected_blocks;
  };
  constexpr Case kCases[] = {
      {0, 0, 0},
      {1, 0, 1},
      {512, 0, 1},
      {513, 0, 1},
      {64 * 1024, 0, 1},
      // Existing/future durable allocation accounting takes precedence over
      // the fallback.
      {64 * 1024, 128, 128},
      // A sparse logical size must not be expanded into allocation from size.
      {1638400 + 51200, 0, 1},
  };

  for (const auto &test_case : kCases) {
    SCOPED_TRACE(test_case.size);
    SwordFsAttr attr(7, S_IFREG | 0644, 100, 200);
    attr.size = test_case.size;
    attr.blocks = test_case.stored_blocks;

    struct stat st{};
    attr.ToPosixStat(&st);

    EXPECT_EQ(st.st_blocks, test_case.expected_blocks);
  }
}

TEST(SwordFsAttrTest, NonRegularPosixBlocksKeepStoredValue) {
  SwordFsAttr attr(7, S_IFDIR | 0755, 100, 200);
  attr.blocks = 9;

  struct stat st{};
  attr.ToPosixStat(&st);

  EXPECT_EQ(st.st_blocks, 9);
}

}  // namespace
