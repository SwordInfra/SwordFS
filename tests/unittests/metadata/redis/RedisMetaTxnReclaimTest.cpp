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

#include "chunk/ChunkObjectKey.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"

namespace swordfs::metadata {

TEST(RedisMetaTxnTest, PrepareReclaimScansAuthoritativeChunksInsideTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("reclaim-authoritative-scan"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.nlink = 0;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 1, 4096};
  std::string chunk_data;
  ASSERT_TRUE(chunk.SerializeTo(&chunk_data).ok());
  redis.hset(key.Chunk(file.ino), "0", chunk_data);

  std::optional<ReclaimWork> work;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.PrepareReclaim(file.ino, work);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, 4096, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, chunk.index, chunk.revision));
  EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
  EXPECT_FALSE(redis.exists(key.Chunk(file.ino)));
  EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
}

TEST(RedisMetaTxnTest, LinkIgnoresUnrelatedFrozenWorkChanges) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  constexpr InodeID kUnrelatedIno = 10;
  const redis::RedisKey key(config.db, UniqueRedisName("link-unrelated-reclaim"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kRootInodeId, root_attr, kRootInodeId)).ok());
  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.Orphans(), std::to_string(kIno), "1");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    SwordFsInode parent;
    auto status = txn.LookupInode(kRootInodeId, &parent);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode file;
    status = txn.LookupInode(kIno, &file);
    if (!status.ok()) {
      return status;
    }
    status = txn.LinkExistingEntry(kRootInodeId, "revived", &parent, &file);
    if (!status.ok()) {
      return status;
    }

    // Change a different field after Link has made every decision but before
    // EXEC. Per-inode reclaim state must not make this unrelated field part
    // of Link's optimistic transaction snapshot.
    redis.hset(key.Reclaims(), std::to_string(kUnrelatedIno), "unrelated-" + std::to_string(attempts));
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  // One attempt is the contract: unrelated fields in the shared reclaims Hash
  // must stay outside this inode's optimistic WATCH snapshot.
  EXPECT_EQ(attempts, 1);
  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "revived"));
}

TEST(RedisMetaTxnTest, PrepareReclaimIgnoresUnrelatedFrozenWorkChanges) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  constexpr InodeID kUnrelatedIno = 10;
  const redis::RedisKey key(config.db, UniqueRedisName("reclaim-unrelated-reclaim"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.Orphans(), std::to_string(kIno), "1");

  RedisMetaClient store(config);
  int attempts = 0;
  std::optional<ReclaimWork> work;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    auto status = txn.PrepareReclaim(kIno, work);
    if (!status.ok()) {
      return status;
    }
    redis.hset(key.Reclaims(), std::to_string(kUnrelatedIno), "unrelated-" + std::to_string(attempts));
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  // One attempt proves an unrelated frozen record does not create false
  // contention for this inode's reclaim transaction.
  EXPECT_EQ(attempts, 1);
  ASSERT_TRUE(work.has_value());
  EXPECT_FALSE(redis.exists(key.Inode(kIno)));
  EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(kIno)));
}

TEST(RedisMetaTxnTest, LinkCommitInvalidatesConcurrentReclaimSnapshot) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  const redis::RedisKey key(config.db, UniqueRedisName("link-reclaim-race"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kRootInodeId, root_attr, kRootInodeId)).ok());
  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.Orphans(), std::to_string(kIno), "1");
  const SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  std::string encoded_chunk;
  ASSERT_TRUE(chunk.SerializeTo(&encoded_chunk).ok());
  redis.hset(key.Chunk(kIno), "0", encoded_chunk);

  RedisMetaClient reclaim_store(config);
  RedisMetaClient link_store(config);
  int attempts = 0;
  std::optional<ReclaimWork> work;
  const auto status = reclaim_store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    auto status = txn.PrepareReclaim(kIno, work);
    if (!status.ok()) {
      return status;
    }
    if (attempts != 1) {
      return utils::Status::OK();
    }

    return link_store.Transact([&](RedisKvTxn &link_kv_txn) {
      RedisMetaTxn link_txn(link_kv_txn, key, 4096);
      SwordFsInode parent;
      auto status = link_txn.LookupInode(kRootInodeId, &parent);
      if (!status.ok()) {
        return status;
      }
      SwordFsInode file;
      status = link_txn.LookupInode(kIno, &file);
      if (!status.ok()) {
        return status;
      }
      return link_txn.LinkExistingEntry(kRootInodeId, "revived", &parent, &file);
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  // Two attempts are intentional: the same-inode Link commit must invalidate
  // the reclaim snapshot once, proving this watched state is the serialization
  // point between Link and reclaim.
  EXPECT_EQ(attempts, 2);
  EXPECT_FALSE(work.has_value());
  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "revived"));
  EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  EXPECT_TRUE(redis.exists(key.Chunk(kIno)));
  EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(kIno)));
}

TEST(RedisMetaTxnTest, PrepareReclaimConvergesAfterAmbiguousExec) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  const redis::RedisKey key(config.db, UniqueRedisName("ambiguous-reclaim"));
  sw::redis::Redis control(ConnectionOptions(config));

  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(control, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  control.hset(key.Orphans(), std::to_string(kIno), "1");
  const SwordFsChunk chunk{.index = 0, .revision = 7, .size = 128};
  std::string encoded_chunk;
  ASSERT_TRUE(chunk.SerializeTo(&encoded_chunk).ok());
  control.hset(key.Chunk(kIno), "0", encoded_chunk);

  config.socket_timeout = std::chrono::milliseconds(50);
  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);
  RedisMetaClient first_store(config);
  int attempts = 0;
  std::optional<ReclaimWork> first_work;
  const auto first_status = first_store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    auto status = txn.PrepareReclaim(kIno, first_work);
    if (!status.ok()) {
      return status;
    }
    control.command<void>("CLIENT", "PAUSE", 1000, "ALL");
    return utils::Status::OK();
  });
  control.command<void>("CLIENT", "UNPAUSE");

  EXPECT_EQ(first_status.ToErrno(), EIO);
  EXPECT_NE(first_status.message().find("ambiguous after EXEC"), std::string::npos);
  EXPECT_EQ(attempts, 1);

  RedisMetaConfig retry_config;
  ASSERT_TRUE(ParseTestConfig(&retry_config));
  RedisMetaClient retry_store(retry_config);
  std::optional<ReclaimWork> replay;
  const auto retry_status = retry_store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.PrepareReclaim(kIno, replay);
  });

  ASSERT_TRUE(retry_status.ok()) << retry_status.message();
  ASSERT_TRUE(replay.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*replay, 4096, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_FALSE(control.exists(key.Inode(kIno)));
  EXPECT_FALSE(control.exists(key.Chunk(kIno)));
  EXPECT_TRUE(control.hexists(key.Reclaims(), std::to_string(kIno)));
}
}  // namespace swordfs::metadata
