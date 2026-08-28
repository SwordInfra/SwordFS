// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "metadata/redis/RedisKey.hpp"

namespace swordfs::metadata::redis {

TEST(RedisKeyTest, UsesDatabaseScopedPrefix) {
  RedisKey key(3, "volume");

  EXPECT_EQ(key.Format(), "{3:volume}:format");
  EXPECT_EQ(key.NextIno(), "{3:volume}:next_ino");
  EXPECT_EQ(key.Inode(42), "{3:volume}:inode:42");
  EXPECT_EQ(key.Directory(7), "{3:volume}:dir:7");
  EXPECT_EQ(key.Chunk(42), "{3:volume}:chunk:42");
  EXPECT_EQ(key.InodeCount(), "{3:volume}:inode_count");
}

TEST(RedisKeyTest, DirectoryKeyDoesNotContainEntryName) {
  RedisKey key(0, "volume");

  // Entry names are fields inside the directory Hash, not part of the Redis
  // key. This keeps all entries of one directory in one Redis object.
  EXPECT_EQ(key.Directory(1), "{0:volume}:dir:1");
}

}  // namespace swordfs::metadata::redis
