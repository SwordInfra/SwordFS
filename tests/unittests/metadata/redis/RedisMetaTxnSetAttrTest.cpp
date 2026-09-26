// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"

namespace swordfs::metadata {

TEST(RedisMetaTxnTest, SetAttrShrinkWorksWithoutDetachedChunkOutput) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-no-cleanup-output"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 4096};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  SwordFsAttr requested = file.attr;
  requested.size = 0;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kSize);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Chunk(file.ino), "0"));

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.size, 0U);
}

TEST(RedisMetaTxnTest, SetAttrSizeAndOwnerChangesDoNotInventKillpriv) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-no-implicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID, 1000, 100);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsAttr requested = file.attr;
  requested.uid = 2000;
  requested.gid = 200;
  requested.size = 1024;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kUid | SetAttrField::kGid | SetAttrField::kSize);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.uid, requested.uid);
  EXPECT_EQ(stored.attr.gid, requested.gid);
  EXPECT_EQ(stored.attr.size, requested.size);
  EXPECT_NE(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, SetAttrExplicitKillprivUsesModeAwareSgidRule) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-explicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, file.attr, kKillSuidGidField);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);

  file.attr = stored.attr;
  file.attr.mode |= S_ISUID | S_ISGID | S_IXGRP;
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, file.attr, kKillSuidGidField);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, SetAttrCombinedModeOwnerAndKillprivUsesFinalRequestedMode) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-combined-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0600, 1000, 100);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsAttr requested = file.attr;
  requested.mode = S_IFREG | 0654 | S_ISUID | S_ISGID;
  requested.uid = 2000;
  requested.gid = 200;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested,
                       SetAttrField::kMode | SetAttrField::kUid | SetAttrField::kGid | kKillSuidGidField);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.uid, requested.uid);
  EXPECT_EQ(stored.attr.gid, requested.gid);
  EXPECT_EQ(stored.attr.mode & 0777, 0654U);
  EXPECT_EQ(stored.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, SetAttrLegacyExplicitModeIsNotSecondGuessed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-legacy-mode"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0600, 1000, 100);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsAttr requested = file.attr;
  requested.mode = S_IFREG | 0644 | S_ISGID;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kMode);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.mode, static_cast<uint32_t>(requested.mode));
}

TEST(RedisMetaTxnTest, TruncateDoesNotInventKillpriv) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-no-implicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 1024);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.size, 1024U);
  EXPECT_NE(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, CommitChunkDoesNotInventKillpriv) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-no-implicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  const SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  const auto status = CommitChunkTxn(store, key, 4096, file.ino, std::nullopt, chunk);
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_NE(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);
}
}  // namespace swordfs::metadata
