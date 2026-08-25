// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "metadata/redis/RedisKey.hpp"

namespace swordfs::metadata {

TEST(RedisKeyTest, UsesDatabaseScopedPrefix) {
  RedisKey key(3, "volume");

  EXPECT_EQ(key.Prefix(), "swordfs:{3:766f6c756d65}:");
  EXPECT_EQ(key.Format(), "swordfs:{3:766f6c756d65}:format");
  EXPECT_EQ(key.NextIno(), "swordfs:{3:766f6c756d65}:next_ino");
  EXPECT_EQ(key.Inode(42), "swordfs:{3:766f6c756d65}:inode:42");
  EXPECT_EQ(key.Dentry(7, "hello"), "swordfs:{3:766f6c756d65}:dentry:7:68656c6c6f");
  EXPECT_EQ(key.DentryPrefix(7), "swordfs:{3:766f6c756d65}:dentry:7:");
  EXPECT_EQ(key.Chunk(42, 3), "swordfs:{3:766f6c756d65}:chunk:42:3");
  EXPECT_EQ(key.ChunkPrefix(42), "swordfs:{3:766f6c756d65}:chunk:42:");
}

TEST(RedisKeyTest, EncodesArbitraryDirectoryNames) {
  RedisKey key(0, "volume");

  EXPECT_EQ(key.Dentry(1, "a:b"), "swordfs:{0:766f6c756d65}:dentry:1:613a62");
  EXPECT_EQ(key.Dentry(1, std::string("\0\xff", 2)), "swordfs:{0:766f6c756d65}:dentry:1:00ff");
}

}  // namespace swordfs::metadata
