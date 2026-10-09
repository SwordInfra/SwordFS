// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

TEST(RedisMetaOpsTest, BackendContextMayBeReleasedByFiberAfterThreadShutdown) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  auto ops = std::make_unique<RedisMetaOps>(config, UniqueRedisName("lifecycle"));
  std::shared_ptr<DirIterator> iterator;
  RunInFiber([&] {
    ASSERT_TRUE(ops->CreateDirIterator(kRootInodeId, {}, &iterator).ok());
    ASSERT_NE(iterator, nullptr);
  });

  // RedisMetaOps is the thread-domain lifecycle owner. Its destructor drains
  // and clears the blocking backend resources while the iterator may still be
  // alive. Releasing the last iterator/context reference on a fiber must then
  // be safe because no blocking resource remains to be destroyed there.
  ops.reset();
  RunInFiber([&] { iterator.reset(); });
}

TEST(RedisMetaOpsTest, TransactionCallbackRunsInThreadDomain) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaOps ops(config, UniqueRedisName("callback-domain"));
  utils::ExecutionDomain callback_domain = utils::ExecutionDomain::kFiber;
  const auto status = RunInFiber([&] {
    return ops.TransactFromFiber([&](RedisMetaTxn &) {
      callback_domain = utils::CurrentExecutionDomain();
      return utils::Status::OK();
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(callback_domain, utils::ExecutionDomain::kThread);
}

TEST(RedisMetaOpsTest, GetInodeUsesDirectMetadataReadPath) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inode");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr attr(42, S_IFREG | 0644);
  attr.size = 1234;
  SwordFsInode inode(42, attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, inode).ok());

  SwordFsInode loaded;
  const auto status = RunInFiber([&] { return ops.GetInode(inode.ino, &loaded); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.ino, inode.ino);
  EXPECT_EQ(loaded.attr.size, inode.attr.size);

  const auto invalid_status = RunInFiber([&] { return ops.GetInode(inode.ino, nullptr); });
  EXPECT_EQ(invalid_status.ToErrno(), EINVAL);
}

TEST(RedisMetaOpsTest, TypedFileMetadataSnapshotsAndAttachmentRejectInvalidTargets) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-typed-attachments");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  constexpr InodeID kFileIno = 42;
  constexpr InodeID kMissingIno = 999999;
  SwordFsInode file(kFileIno, SwordFsAttr(kFileIno, S_IFREG | 0644), kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  RunInFiber([&] {
    FileChunkSnapshot snapshot;
    FileMappingSnapshot mapping;
    std::optional<ChunkID> attached;
    EXPECT_EQ(ops.ReadFileChunkSnapshot(kFileIno, 0, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(ops.ReadFileMappingSnapshot(kFileIno, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(ops.ProbeAttachment(kFileIno, 0, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(ops.ReadFileChunkSnapshot(kMissingIno, 0, &snapshot).ToErrno(), ENOENT);
    EXPECT_EQ(ops.ReadFileMappingSnapshot(kMissingIno, &mapping).ToErrno(), ENOENT);
    EXPECT_EQ(ops.ProbeAttachment(kMissingIno, 0, &attached).ToErrno(), ENOENT);
    ASSERT_TRUE(ops.ReadFileChunkSnapshot(kFileIno, 0, &snapshot).ok());
    EXPECT_FALSE(snapshot.chunk_id.has_value());
    ASSERT_TRUE(ops.ReadFileMappingSnapshot(kFileIno, &mapping).ok());
    EXPECT_TRUE(mapping.mappings.empty());
    ASSERT_TRUE(ops.ProbeAttachment(kFileIno, 0, &attached).ok());
    EXPECT_FALSE(attached.has_value());

    const ChunkID id(1001);
    EXPECT_EQ(ops.AttachPrepared(kFileIno, 0, ChunkID(0), 16, {.eof = 0}).ToErrno(), EINVAL);
    ASSERT_TRUE(ops.AttachPrepared(kFileIno, 0, id, 16, {.eof = 0}).ok());
    EXPECT_EQ(ops.AttachPrepared(kFileIno, 0, ChunkID(1002), 16, {.eof = 0}).ToErrno(), EEXIST);
    ASSERT_TRUE(ops.ReadFileChunkSnapshot(kFileIno, 1, &snapshot).ok());
    ASSERT_TRUE(snapshot.eof_boundary.has_value());
    EXPECT_EQ(snapshot.eof_boundary->index, 0U);
    EXPECT_EQ(snapshot.eof_boundary->chunk_id, id);
    ASSERT_TRUE(ops.ReadFileMappingSnapshot(kFileIno, &mapping).ok());
    ASSERT_EQ(mapping.mappings.size(), 1U);
    EXPECT_EQ(mapping.mappings[0].chunk_id, id);
    ASSERT_TRUE(ops.ProbeAttachment(kFileIno, 0, &attached).ok());
    EXPECT_EQ(attached, id);

    ChunkSizePlan grow;
    ASSERT_TRUE(PlanChunkSizeChange(16, 32, SwordFsVolume{}.chunk_size, mapping.mappings, &grow).ok());
    SwordFsAttr requested;
    requested.size = 32;
    ChunkSizeCommitResult committed;
    EXPECT_EQ(ops.CommitGrow(kFileIno, grow, requested, SetAttrField::kSize, nullptr).ToErrno(), EINVAL);
    ASSERT_TRUE(ops.CommitGrow(kFileIno, grow, requested, SetAttrField::kSize, &committed).ok());
    EXPECT_EQ(committed.inode.attr.size, 32U);
  });
}

TEST(RedisMetaOpsTest, GetInodesUsesAlignedBatchReadWithMissingEntries) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inodes");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsInode first(42, SwordFsAttr(42, S_IFREG | 0644), kRootInodeId);
  first.attr.size = 1234;
  SwordFsInode second(43, SwordFsAttr(43, S_IFDIR | 0750), kRootInodeId);
  second.attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, first).ok());
  ASSERT_TRUE(SeedInode(redis, key, second).ok());

  std::vector<std::optional<SwordFsInode>> results;
  const auto status = RunInFiber([&] { return ops.GetInodes({first.ino, 999999, second.ino}, &results); });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(results.size(), 3u);
  ASSERT_TRUE(results[0].has_value());
  EXPECT_EQ(results[0]->attr.size, first.attr.size);
  EXPECT_FALSE(results[1].has_value());
  ASSERT_TRUE(results[2].has_value());
  EXPECT_EQ(results[2]->attr.nlink, second.attr.nlink);

  results.emplace_back(first);
  ASSERT_TRUE(RunInFiber([&] { return ops.GetInodes({}, &results); }).ok());
  EXPECT_TRUE(results.empty());

  const auto invalid_status = RunInFiber([&] { return ops.GetInodes({first.ino}, nullptr); });
  EXPECT_EQ(invalid_status.ToErrno(), EINVAL);
}

TEST(RedisMetaOpsTest, GetInodesPropagatesConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaOps ops(config, "unreachable-get-inodes");
  std::vector<std::optional<SwordFsInode>> results;
  const auto status = RunInFiber([&] { return ops.GetInodes({42}, &results); });
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
}

TEST(RedisMetaOpsTest, PrepareReclaimPropagatesConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaOps ops(config, "unreachable-prepare-reclaim");
  const auto status = RunInFiber([&] { return ops.PrepareReclaim(42); });
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
}

TEST(RedisMetaOpsTest, GetInodesRejectsMalformedAndMismatchedRecords) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inodes-invalid");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  constexpr InodeID kMalformedIno = 44;
  redis.set(key.Inode(kMalformedIno), "not-an-inode");
  std::vector<std::optional<SwordFsInode>> results;
  EXPECT_TRUE(RunInFiber([&] { return ops.GetInodes({kMalformedIno}, &results); }).ToErrno() == EIO);

  constexpr InodeID kRequestedIno = 46;
  SwordFsInode other(45, SwordFsAttr(45, S_IFREG | 0644), kRootInodeId);
  std::string encoded;
  ASSERT_TRUE(other.SerializeTo(&encoded).ok());
  redis.set(key.Inode(kRequestedIno), encoded);
  EXPECT_TRUE(RunInFiber([&] { return ops.GetInodes({kRequestedIno}, &results); }).ToErrno() == EIO);
}

TEST(RedisMetaOpsTest, LookupEntryOwnsReadOnlyTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-lookup-entry");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr child_attr(2, S_IFREG | 0644);
  SwordFsInode child(2, child_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, child).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"child", DT_REG, child.ino}).ok());

  SwordFsInode loaded;
  const auto status = RunInFiber([&] { return ops.LookupEntry(kRootInodeId, "child", &loaded); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.ino, child.ino);

  const auto invalid_status = RunInFiber([&] { return ops.LookupEntry(kRootInodeId, "child", nullptr); });
  EXPECT_EQ(invalid_status.ToErrno(), EINVAL);
}
}  // namespace swordfs::metadata
