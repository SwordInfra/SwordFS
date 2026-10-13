// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "metadata/ChunkSizePlan.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"

namespace swordfs::metadata {

TEST(RedisMetaTxnTest, TypedFileContractRejectsInvalidTransactionInputs) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("typed-txn-contract-validation"));
  sw::redis::Redis redis(ConnectionOptions(config));
  constexpr InodeID kFileIno = 41;
  constexpr InodeID kDirectoryIno = 42;
  constexpr InodeID kMissingIno = 43;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kFileIno, SwordFsAttr(kFileIno, S_IFREG | 0644), kRootInodeId)).ok());
  ASSERT_TRUE(
      SeedInode(redis, key, SwordFsInode(kDirectoryIno, SwordFsAttr(kDirectoryIno, S_IFDIR | 0755), kRootInodeId))
          .ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &COWEncoderForTest());
    FileChunkSnapshot chunk;
    FileMappingSnapshot mappings;
    std::optional<ChunkID> attached;
    ChunkSizePlan plan;
    SwordFsAttr requested;
    requested.size = 0;
    ChunkSizeCommitResult committed;

    EXPECT_EQ(txn.ReadFileChunkSnapshot(kFileIno, 0, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ReadFileMappingSnapshot(kFileIno, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ProbeAttachment(kFileIno, 0, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.CommitShrink(kFileIno, plan, requested, SetAttrField::kSize, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.CommitGrow(kFileIno, plan, requested, SetAttrField::kMode, &committed).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ReadFileChunkSnapshot(kDirectoryIno, 0, &chunk).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ReadFileMappingSnapshot(kDirectoryIno, &mappings).ToErrno(), EINVAL);
    EXPECT_EQ(txn.AttachPrepared(kDirectoryIno, 0, ChunkID(17), 8, {.eof = 0}).ToErrno(), EINVAL);
    EXPECT_EQ(txn.FinalizeAttachedWrite(kDirectoryIno, 0, ChunkID(17), 8, {.eof = 0}).ToErrno(), EINVAL);
    EXPECT_TRUE(txn.ReadFileChunkSnapshot(kMissingIno, 0, &chunk).IsNotFound());
    EXPECT_TRUE(txn.ReadFileMappingSnapshot(kMissingIno, &mappings).IsNotFound());
    EXPECT_TRUE(txn.ProbeAttachment(kMissingIno, 0, &attached).IsNotFound());
    EXPECT_TRUE(txn.AttachPrepared(kMissingIno, 0, ChunkID(17), 8, {.eof = 0}).IsNotFound());
    EXPECT_TRUE(txn.FinalizeAttachedWrite(kMissingIno, 0, ChunkID(17), 8, {.eof = 0}).IsNotFound());
    EXPECT_TRUE(txn.CommitShrink(kMissingIno, plan, requested, SetAttrField::kSize, &committed).IsNotFound());
    EXPECT_TRUE(txn.CommitGrow(kMissingIno, plan, requested, SetAttrField::kSize, &committed).IsNotFound());
    return utils::Status::OK();
  });
  ASSERT_TRUE(status.ok()) << status.message();
}

TEST(RedisMetaTxnTest, RegisterPendingDeletesRejectsInvalidEnvelope) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("register-invalid-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const std::vector<PendingDelete> work{{.id = "", .payload = "opaque"}};

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &COWEncoderForTest());
    return txn.RegisterPendingDeletes(work);
  });
  EXPECT_EQ(status.ToErrno(), EINVAL);
  EXPECT_EQ(redis.hlen(key.PendingDeletes()), 0);
}

TEST(RedisMetaTxnTest, TypedShrinkKnownPartialExecStillCommitsEofAndDetach) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("typed-shrink-partial-exec"));
  sw::redis::Redis redis(ConnectionOptions(config));
  constexpr InodeID kFileIno = 41;
  constexpr uint64_t kChunkSize = 4096;
  SwordFsAttr file_attr(kFileIno, S_IFREG | 0644);
  file_attr.size = 2 * kChunkSize;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kFileIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.ChunkRefs(kFileIno), "0", "1001");
  redis.hset(key.ChunkRefs(kFileIno), "1", "1002");
  redis.set(key.PendingDeletes(), "wrong-type");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, kChunkSize, &COWEncoderForTest());
    FileMappingSnapshot before;
    auto step = txn.ReadFileMappingSnapshot(kFileIno, &before);
    if (!step.ok()) {
      return step;
    }
    ChunkSizePlan plan;
    step = PlanChunkSizeChange(before.inode.attr.size, 0, kChunkSize, before.mappings, &plan);
    if (!step.ok()) {
      return step;
    }
    SwordFsAttr requested{};
    requested.size = 0;
    ChunkSizeCommitResult committed;
    return txn.CommitShrink(kFileIno, plan, requested, SetAttrField::kSize, &committed);
  });
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_FALSE(status.IsOutcomeUnknown());
  EXPECT_NE(status.message().find("WRONGTYPE"), std::string::npos);

  FileMappingSnapshot after;
  const auto readback = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, kChunkSize, &COWEncoderForTest());
    return txn.ReadFileMappingSnapshot(kFileIno, &after);
  });
  ASSERT_TRUE(readback.ok()) << readback.message();
  EXPECT_EQ(after.inode.attr.size, 0U);
  EXPECT_TRUE(after.mappings.empty());
  EXPECT_EQ(redis.get(key.PendingDeletes()), std::optional<std::string>{"wrong-type"});
}

}  // namespace swordfs::metadata
