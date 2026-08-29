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
  EXPECT_EQ(key.Dentry(7, "hello"), "{3:volume}:dentry:7:hello");
  EXPECT_EQ(key.Chunk(42, 3), "{3:volume}:chunk:42:3");
}

TEST(RedisKeyTest, PreservesDirectoryNames) {
  RedisKey key(0, "volume");

  EXPECT_EQ(key.Dentry(1, "a:b"), "{0:volume}:dentry:1:a:b");
  EXPECT_EQ(key.Dentry(1, std::string("\0\xff", 2)), "{0:volume}:dentry:1:" + std::string("\0\xff", 2));
}

}  // namespace swordfs::metadata::redis
