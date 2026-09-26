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

TEST(RedisMetaTxnTest, PrivateIndexPublicationCommitsAndRejectsWithLogicalHead) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-publication"));
  sw::redis::Redis redis(ConnectionOptions(config));
  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  RecordingRedisStrategy strategy;

  std::optional<PendingDelete> last_cleanup;
  auto publish = [&](const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement,
                     const ChunkPublishIntent &intent = {}) {
    utils::Status publication_result;
    std::optional<PendingDelete> cleanup_candidate;
    auto status = store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
      return txn.CommitChunk(file.ino, expected, replacement, publication_result, cleanup_candidate, intent);
    });
    last_cleanup = std::move(cleanup_candidate);
    return status.ok() ? publication_result : status;
  };

  const SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(publish(std::nullopt, first, ChunkPublishIntent{.payload = "manifest-one"}).ok());
  const auto private_hash = key.PrivateChunkIndex(strategy.mechanism(), "fragments:" + std::to_string(file.ino));
  EXPECT_EQ(redis.hget(private_hash, "1"), std::optional<std::string>{"manifest-one"});
  std::string private_value;
  std::vector<std::pair<std::string, std::string>> fields;
  ASSERT_TRUE(store
                  .Transact([&](RedisKvTxn &kv_txn) {
                    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
                    auto status = txn.Read("fragments:" + std::to_string(file.ino), "1", &private_value);
                    if (!status.ok()) {
                      return status;
                    }
                    status = txn.Scan("fragments:" + std::to_string(file.ino), &fields);
                    if (!status.ok()) {
                      return status;
                    }
                    ChunkView view;
                    status = txn.LoadChunkView(file.ino, 0, &view);
                    if (!status.ok()) {
                      return status;
                    }
                    EXPECT_EQ(view.head, first);
                    EXPECT_EQ(view.private_snapshot, "manifest-one");
                    return utils::Status::OK();
                  })
                  .ok());
  EXPECT_EQ(private_value, "manifest-one");
  EXPECT_EQ(fields, (std::vector<std::pair<std::string, std::string>>{{"1", "manifest-one"}}));

  const SwordFsChunk replacement{.index = 0, .revision = 2, .size = 96};
  strategy.index.reject_publish = true;
  EXPECT_EQ(publish(first, replacement, ChunkPublishIntent{.payload = "manifest-two"}).ToErrno(), EIO);
  ASSERT_TRUE(last_cleanup.has_value());
  EXPECT_FALSE(redis.hget(private_hash, "2").has_value());
  SwordFsChunk head;
  const auto encoded = redis.hget(key.Chunk(file.ino), "0");
  ASSERT_TRUE(encoded.has_value());
  ASSERT_TRUE(head.ParseFrom(*encoded).ok());
  EXPECT_EQ(head, first);
}

TEST(RedisMetaTxnTest, ChunkPublicContractsRejectInvalidDescriptorsAndViews) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("chunk-contract-validation"));
  sw::redis::Redis redis(ConnectionOptions(config));
  RecordingRedisStrategy strategy;
  constexpr InodeID kFileIno = 40;

  const auto validation_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    utils::Status publication_result;
    std::optional<PendingDelete> cleanup_candidate;

    const SwordFsChunk replacement{.index = 0, .revision = 2, .size = 64};

    const SwordFsChunk same_revision{.index = 0, .revision = 2, .size = 64};
    EXPECT_EQ(txn.CommitChunk(kFileIno, same_revision, replacement, publication_result, cleanup_candidate).ToErrno(),
              EINVAL);

    const SwordFsChunk expected{.index = 0, .revision = 1, .size = 64};
    const SwordFsChunk different_identity{.index = 1, .revision = 2, .size = 64};
    EXPECT_EQ(txn.CommitChunk(kFileIno, expected, different_identity, publication_result, cleanup_candidate).ToErrno(),
              EINVAL);

    EXPECT_EQ(txn.LoadChunkView(kFileIno, 0, nullptr).ToErrno(), EINVAL);
    return utils::Status::OK();
  });
  ASSERT_TRUE(validation_status.ok()) << validation_status.message();

  SwordFsChunk wrong_identity{.index = 0, .revision = 3, .size = 64};
  std::string encoded;
  ASSERT_TRUE(wrong_identity.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(kFileIno), "1", encoded);
  const auto malformed_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    ChunkView view;
    return txn.LoadChunkView(kFileIno, 1, &view);
  });
  EXPECT_EQ(malformed_status.ToErrno(), EIO) << malformed_status.message();

  redis.del(key.Chunk(kFileIno));
  SwordFsChunk head{.index = 0, .revision = 4, .size = 64};
  ASSERT_TRUE(head.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(kFileIno), "0", encoded);
  const auto private_state_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    ChunkView view;
    return txn.LoadChunkView(kFileIno, 0, &view);
  });
  EXPECT_TRUE(private_state_status.IsNotFound()) << private_state_status.message();
}

TEST(RedisMetaTxnTest, PrivateChunkIndexPrimitivesValidateTheirPublicContract) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-contract"));
  sw::redis::Redis redis(ConnectionOptions(config));
  RecordingRedisStrategy strategy;

  const auto validation_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    std::string value;
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_EQ(txn.Read("", "field", &value).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Read("manifest", "field", nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Scan("", &values).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Scan("manifest", nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Put("", "field", "value").ToErrno(), EINVAL);
    EXPECT_EQ(txn.Erase("", "field").ToErrno(), EINVAL);
    return utils::Status::OK();
  });
  ASSERT_TRUE(validation_status.ok()) << validation_status.message();

  const auto put_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    return txn.Put("manifest", "b", "two");
  });
  ASSERT_TRUE(put_status.ok()) << put_status.message();
  redis.hset(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "a", "one");

  const auto read_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    std::string value;
    auto status = txn.Read("manifest", "b", &value);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(value, "two");

    std::vector<std::pair<std::string, std::string>> values;
    status = txn.Scan("manifest", &values);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(values, (std::vector<std::pair<std::string, std::string>>{{"a", "one"}, {"b", "two"}}));
    return utils::Status::OK();
  });
  ASSERT_TRUE(read_status.ok()) << read_status.message();

  const auto erase_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    return txn.Erase("manifest", "a");
  });
  ASSERT_TRUE(erase_status.ok()) << erase_status.message();
  EXPECT_FALSE(redis.hexists(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "a"));
  EXPECT_EQ(redis.hget(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "b").value_or(""), "two");
}

TEST(RedisMetaTxnTest, PrivateChunkIndexScanFailsClosedOnCorruptBackendType) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-wrong-type"));
  sw::redis::Redis redis(ConnectionOptions(config));
  RecordingRedisStrategy strategy;
  redis.set(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "not-a-hash");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    std::vector<std::pair<std::string, std::string>> values;
    return txn.Scan("manifest", &values);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(redis.get(key.PrivateChunkIndex(strategy.mechanism(), "manifest")).value_or(""), "not-a-hash");
}

TEST(RedisMetaTxnTest, TruncateClampsPersistedBoundaryChunk) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-staged"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 8192;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk first_chunk{0, 1, 4096};
  SwordFsChunk second_chunk{1, 2, 4096};
  std::string first_data;
  std::string second_data;
  ASSERT_TRUE(first_chunk.SerializeTo(&first_data).ok());
  ASSERT_TRUE(second_chunk.SerializeTo(&second_data).ok());
  redis.hset(key.Chunk(9), "0", first_data);
  redis.hset(key.Chunk(9), "1", second_data);

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(9, 1024, &detached);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(detached.size(), 1U);
  swordfs::chunk::WholeObjectRef detached_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(detached.front(), 4096, &detached_ref).ok());
  EXPECT_EQ(detached_ref.descriptor, second_chunk);

  const auto first_value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(first_value.has_value());
  SwordFsChunk first;
  ASSERT_TRUE(first.ParseFrom(*first_value).ok());
  EXPECT_EQ(first.size, 1024U);
  EXPECT_FALSE(redis.hexists(key.Chunk(9), "1"));

  file.attr.size = 4096;
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  const auto invalid_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 0);
    EXPECT_EQ(txn.Truncate(9, 1024).ToErrno(), EIO);
    return utils::Status::OK();
  });
  EXPECT_TRUE(invalid_status.ok()) << invalid_status.message();
}

TEST(RedisMetaTxnTest, RegisterPendingDeletesRejectsInvalidEnvelope) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("register-invalid-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const std::vector<PendingDelete> work{{.id = "", .index_format_version = 1, .payload = "opaque"}};

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RegisterPendingDeletes(work);
  });
  EXPECT_EQ(status.ToErrno(), EINVAL);
  EXPECT_EQ(redis.hlen(key.PendingDeletes()), 0);
}

TEST(RedisMetaTxnTest, TruncateDoesNotDependOnPendingDeleteState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-independent-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 1, 4096};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0, &detached);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(detached.size(), 1U);
  swordfs::chunk::WholeObjectRef detached_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(detached.front(), 4096, &detached_ref).ok());
  EXPECT_EQ(detached_ref.descriptor, chunk);
  EXPECT_FALSE(redis.hexists(key.Chunk(file.ino), "0"));
}

TEST(RedisMetaTxnTest, TruncateScansMultipleChunkHashPagesAndCollectsDetachedChunks) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-multipage-chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  constexpr uint64_t kChunkSize = 4096;
  constexpr uint32_t kChunkCount = 600;
  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = kChunkCount * kChunkSize;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  for (uint32_t index = 0; index < kChunkCount; ++index) {
    SwordFsChunk chunk{.index = index,

                       .revision = static_cast<uint64_t>(index) + 1,
                       .size = kChunkSize};
    std::string encoded;
    ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
    redis.hset(key.Chunk(file.ino), std::to_string(index), encoded);
  }

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, kChunkSize);
    return txn.Truncate(file.ino, 0, &detached);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(detached.size(), kChunkCount);
  EXPECT_EQ(redis.hlen(key.Chunk(file.ino)), 0);
}

TEST(RedisMetaTxnTest, TruncateRejectsInvalidChunkMetadata) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-noncanonical-chunk"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk wrong{.index = 0, .revision = 1, .size = 4097};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Chunk(file.ino), "0"));

  redis.del(key.Chunk(file.ino));
  SwordFsChunk canonical{.index = 0, .revision = 2, .size = 64};
  ASSERT_TRUE(canonical.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "1", encoded);
  const auto field_mismatch_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_TRUE(field_mismatch_status.ToErrno() == EIO) << field_mismatch_status.message();
}

TEST(RedisMetaTxnTest, TruncatePropagatesWrongTypeChunkMapFromDestructivePhase) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-wrongtype-chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  redis.set(key.Chunk(file.ino), "wrong-type");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_FALSE(status.ok());

  SwordFsInode after;
  ASSERT_TRUE(after.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(after.attr.size, file.attr.size);
}

TEST(RedisMetaTxnTest, CommitChunkRejectsCorruptPersistedDescriptor) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-corrupt-current"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk corrupt{.index = 0, .revision = 1, .size = 4097};
  std::string encoded;
  ASSERT_TRUE(corrupt.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  SwordFsChunk replacement{.index = 0, .revision = 2, .size = 64};
  const auto status = CommitChunkTxn(store, key, 4096, file.ino, std::nullopt, replacement);
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
}

TEST(RedisMetaTxnTest, CommitChunkReturnsCleanupCandidateWithoutPendingDeleteDependency) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rewrite-cleanup-candidate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 64;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk expected{.index = 0, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(expected.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  auto replacement = expected;
  replacement.revision = 2;
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.CommitChunk(file.ino, expected, replacement, publication_result, cleanup_candidate);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(publication_result.ok()) << publication_result.message();
  ASSERT_TRUE(cleanup_candidate.has_value());
  swordfs::chunk::WholeObjectRef cleanup_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(*cleanup_candidate, 4096, &cleanup_ref).ok());
  EXPECT_EQ(cleanup_ref.descriptor, expected);
  const auto stored = redis.hget(key.Chunk(file.ino), "0");
  ASSERT_TRUE(stored.has_value());
  SwordFsChunk current;
  ASSERT_TRUE(current.ParseFrom(*stored).ok());
  EXPECT_EQ(current, replacement);
}

TEST(RedisMetaTxnTest, CommitChunkDefiniteRejectionReturnsReplacementAsCleanupCandidate) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("reject-cleanup-candidate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 64;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk current{.index = 0, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(current.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  auto replacement = current;
  replacement.revision = 2;
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.CommitChunk(file.ino, std::nullopt, replacement, publication_result, cleanup_candidate);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(publication_result.ToErrno() == EEXIST) << publication_result.message();
  ASSERT_TRUE(cleanup_candidate.has_value());
  swordfs::chunk::WholeObjectRef cleanup_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(*cleanup_candidate, 4096, &cleanup_ref).ok());
  EXPECT_EQ(cleanup_ref.descriptor, replacement);
}

TEST(RedisMetaTxnTest, CommitChunkRejectsConflictingInitialPublication) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-chunk"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 1, 4096};
  const auto first_status = CommitChunkTxn(store, key, 4096, 9, std::nullopt, chunk);
  ASSERT_TRUE(first_status.ok()) << first_status.message();

  chunk.revision = 2;
  const auto duplicate_status = CommitChunkTxn(store, key, 4096, 9, std::nullopt, chunk);
  EXPECT_TRUE(duplicate_status.ToErrno() == EEXIST) << duplicate_status.message();

  const auto value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(value.has_value());
  SwordFsChunk persisted;
  ASSERT_TRUE(persisted.ParseFrom(*value).ok());
  EXPECT_EQ(persisted.revision, 1U);
}
}  // namespace swordfs::metadata
