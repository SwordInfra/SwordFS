// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <cerrno>
#include <memory>
#include <optional>

#include "FiberTest.hpp"
#include "metadata/ChunkMetadata.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisCOWChunkMetadata.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClientFaultServer.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {
namespace {

TEST(RedisCOWChunkMetadataTest, AllocatesOneVolumeScopedIdentitySequenceAcrossTypedViews) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("chunk-id");
  redis::RedisKey key(config.db, volume_name);
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata first_view(backend, key);
  RedisCOWChunkMetadata second_view(backend, key);
  ChunkID first;
  ChunkID second;
  utils::Status first_status;
  utils::Status second_status;
  utils::Status null_status;
  swordfs::test::RunInTestFiber([&] {
    first_status = first_view.AllocateChunkID(&first);
    second_status = second_view.AllocateChunkID(&second);
    null_status = first_view.AllocateChunkID(nullptr);
  });

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(key.NextChunkID());
  backend->Shutdown();

  ASSERT_TRUE(first_status.ok()) << first_status.message();
  ASSERT_TRUE(second_status.ok()) << second_status.message();
  EXPECT_EQ(first_view.Type(), ChunkType::kCow);
  EXPECT_EQ(second_view.Type(), ChunkType::kCow);
  EXPECT_EQ(first, ChunkID(1));
  EXPECT_EQ(second, ChunkID(2));
  EXPECT_EQ(null_status.ToErrno(), EINVAL);
}

TEST(RedisCOWChunkMetadataTest, RejectsNegativeCounterWithoutMutatingIt) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("chunk-id-negative");
  redis::RedisKey key(config.db, volume_name);
  const auto chunk_id_key = key.NextChunkID();
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(chunk_id_key, "-1");

  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, key);
  ChunkID first;
  ChunkID second;
  utils::Status first_status;
  utils::Status second_status;
  swordfs::test::RunInTestFiber([&] {
    first_status = metadata.AllocateChunkID(&first);
    second_status = metadata.AllocateChunkID(&second);
  });
  const auto persisted = redis.get(chunk_id_key);
  redis.del(chunk_id_key);
  backend->Shutdown();

  EXPECT_EQ(first_status.ToErrno(), EIO);
  EXPECT_EQ(second_status.ToErrno(), EIO);
  EXPECT_EQ(first, kInvalidChunkID);
  EXPECT_EQ(second, kInvalidChunkID);
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(*persisted, "-1");
}

TEST(RedisCOWChunkMetadataTest, ExhaustionFailsClosedWithoutAdvancingCounter) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("chunk-id-exhaustion");
  redis::RedisKey key(config.db, volume_name);
  const auto chunk_id_key = key.NextChunkID();
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(chunk_id_key, std::to_string(kMaxChunkIDValue));

  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, key);
  ChunkID first;
  ChunkID second;
  utils::Status first_status;
  utils::Status second_status;
  swordfs::test::RunInTestFiber([&] {
    first_status = metadata.AllocateChunkID(&first);
    second_status = metadata.AllocateChunkID(&second);
  });
  const auto persisted = redis.get(chunk_id_key);
  redis.del(chunk_id_key);
  backend->Shutdown();

  EXPECT_EQ(first_status.ToErrno(), EIO);
  EXPECT_EQ(second_status.ToErrno(), EIO);
  EXPECT_EQ(first, kInvalidChunkID);
  EXPECT_EQ(second, kInvalidChunkID);
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(*persisted, std::to_string(kMaxChunkIDValue));
}

TEST(RedisCOWChunkMetadataTest, AmbiguousIncrOutcomePropagatesWithoutReplay) {
  for (const auto scenario :
       {RedisFaultScenario::kIncrProtocolFailureApplied, RedisFaultScenario::kIncrDisconnectNotApplied}) {
    ScriptedRedisServer server(scenario);
    auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
    redis::RedisKey key(0, "chunk-id-fault");
    RedisCOWChunkMetadata metadata(backend, key);
    ChunkID id;
    utils::Status status;

    swordfs::test::RunInTestFiber([&] { status = metadata.AllocateChunkID(&id); });
    server.Wait();
    backend->Shutdown();

    EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
    EXPECT_EQ(id, kInvalidChunkID);
    EXPECT_TRUE(server.error().empty()) << server.error();
    EXPECT_EQ(server.mutation_applied(), scenario == RedisFaultScenario::kIncrProtocolFailureApplied);
  }
}

TEST(RedisCOWChunkMetadataTest, AmbiguousAppliedAllocationLeavesGapAndRetryGetsFreshID) {
  ScriptedRedisServer server(RedisFaultScenario::kIncrProtocolFailureAppliedThenFreshValue);
  auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
  redis::RedisKey key(0, "chunk-id-fresh-retry");
  RedisCOWChunkMetadata metadata(backend, key);
  ChunkID lost;
  ChunkID fresh;
  utils::Status lost_status;
  utils::Status fresh_status;

  swordfs::test::RunInTestFiber([&] {
    lost_status = metadata.AllocateChunkID(&lost);
    fresh_status = metadata.AllocateChunkID(&fresh);
  });
  server.Wait();
  backend->Shutdown();

  EXPECT_TRUE(lost_status.IsOutcomeUnknown()) << lost_status.message();
  ASSERT_TRUE(fresh_status.ok()) << fresh_status.message();
  EXPECT_EQ(lost, kInvalidChunkID);
  EXPECT_EQ(fresh, ChunkID(2));
  EXPECT_TRUE(server.mutation_applied());
  EXPECT_EQ(server.connection_count(), 2);
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisCOWChunkMetadataTest, AmbiguousAppliedRevisionAllocationLeavesGapAndRetryGetsFreshRevision) {
  ScriptedRedisServer server(RedisFaultScenario::kIncrProtocolFailureAppliedThenFreshValue);
  auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
  RedisCOWChunkMetadata metadata(backend, redis::RedisKey(0, "cow-revision-fresh-retry"));
  cow::COWChunkRevision lost;
  cow::COWChunkRevision fresh;
  utils::Status lost_status;
  utils::Status fresh_status;

  swordfs::test::RunInTestFiber([&] {
    lost_status = metadata.AllocateRevision(ChunkID(42), &lost);
    fresh_status = metadata.AllocateRevision(ChunkID(42), &fresh);
  });
  server.Wait();
  backend->Shutdown();

  EXPECT_TRUE(lost_status.IsOutcomeUnknown()) << lost_status.message();
  ASSERT_TRUE(fresh_status.ok()) << fresh_status.message();
  EXPECT_EQ(lost, cow::kInvalidCOWChunkRevision);
  EXPECT_EQ(fresh, cow::COWChunkRevision(2));
  EXPECT_TRUE(server.mutation_applied());
  EXPECT_EQ(server.connection_count(), 2);
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisCOWChunkMetadataTest, StoresTypedHeadsWithPerChunkRevisionAllocationAndFullHeadCas) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("cow-head");
  redis::RedisKey key(config.db, volume_name);
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, key);
  ChunkID first_id;
  ChunkID second_id;
  cow::COWChunkRevision first_revision;
  cow::COWChunkRevision second_revision;
  cow::COWChunkRevision other_chunk_revision;
  cow::COWChunkHead loaded;

  swordfs::test::RunInTestFiber([&] {
    ASSERT_TRUE(metadata.AllocateChunkID(&first_id).ok());
    ASSERT_TRUE(metadata.AllocateChunkID(&second_id).ok());
    ASSERT_TRUE(metadata.AllocateRevision(first_id, &first_revision).ok());
    ASSERT_TRUE(metadata.AllocateRevision(first_id, &second_revision).ok());
    ASSERT_TRUE(metadata.AllocateRevision(second_id, &other_chunk_revision).ok());
    EXPECT_EQ(first_revision, cow::COWChunkRevision(1));
    EXPECT_EQ(second_revision, cow::COWChunkRevision(2));
    EXPECT_EQ(other_chunk_revision, cow::COWChunkRevision(1));

    const cow::COWChunkHead first_head{first_revision, 4096};
    ASSERT_TRUE(metadata.CompareExchangeHead(first_id, std::nullopt, first_head).ok());
    EXPECT_EQ(metadata.CompareExchangeHead(first_id, std::nullopt, first_head).ToErrno(), EEXIST);
    ASSERT_TRUE(metadata.GetHead(first_id, &loaded).ok());
    EXPECT_EQ(loaded, first_head);

    const cow::COWChunkHead stale_same_revision{first_revision, 2048};
    const cow::COWChunkHead rewritten{second_revision, 3072};
    EXPECT_EQ(metadata.CompareExchangeHead(first_id, stale_same_revision, rewritten).ToErrno(), EEXIST);
    EXPECT_EQ(metadata.CompareExchangeHead(first_id, first_head, cow::COWChunkHead{first_revision, 8192}).ToErrno(),
              EINVAL);
    ASSERT_TRUE(metadata.CompareExchangeHead(first_id, first_head, rewritten).ok());
    const cow::COWChunkHead clamped{second_revision, 1024};
    ASSERT_TRUE(metadata.CompareExchangeHead(first_id, rewritten, clamped).ok());
    ASSERT_TRUE(metadata.GetHead(first_id, &loaded).ok());
    EXPECT_EQ(loaded, clamped);
    EXPECT_EQ(metadata.EraseHead(first_id, rewritten).ToErrno(), EEXIST);
    ASSERT_TRUE(metadata.EraseHead(first_id, clamped).ok());
    EXPECT_TRUE(metadata.GetHead(first_id, &loaded).IsNotFound());
    cow::COWChunkRevision post_erase_revision;
    ASSERT_TRUE(metadata.AllocateRevision(first_id, &post_erase_revision).ok());
    EXPECT_EQ(post_erase_revision, cow::COWChunkRevision(3));
    const cow::COWChunkHead post_erase_rewrite{post_erase_revision, 512};
    EXPECT_TRUE(metadata.CompareExchangeHead(first_id, clamped, post_erase_rewrite).IsNotFound());
    EXPECT_TRUE(metadata.EraseHead(first_id, clamped).IsNotFound());
  });

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(key.NextChunkID());
  backend->Shutdown();
}

TEST(RedisCOWChunkMetadataTest, MalformedPersistedTypedHeadFailsClosed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("cow-head-malformed");
  redis::RedisKey key(config.db, volume_name);
  const ChunkID chunk_id(42);
  const auto head_key = key.PrivateChunkIndex(ChunkType::kCow, "head:" + std::to_string(chunk_id.Value()));
  sw::redis::Redis redis(ConnectionOptions(config));
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, key);

  cow::COWChunkHead head;
  utils::Status malformed_status;
  utils::Status invalid_revision_status;
  redis.set(head_key, "not-a-cow-head");
  swordfs::test::RunInTestFiber([&] { malformed_status = metadata.GetHead(chunk_id, &head); });

  BufEncoder encoder;
  encoder.U64(cow::kMaxCOWChunkRevisionValue + 1);
  encoder.U64(1);
  std::string invalid_revision_head;
  encoder.Finish(&invalid_revision_head);
  redis.set(head_key, invalid_revision_head);
  swordfs::test::RunInTestFiber([&] { invalid_revision_status = metadata.GetHead(chunk_id, &head); });

  backend->Shutdown();
  redis.del(head_key);

  EXPECT_EQ(malformed_status.ToErrno(), EIO);
  EXPECT_EQ(invalid_revision_status.ToErrno(), EIO);
  EXPECT_EQ(head, cow::COWChunkHead{});
}

TEST(RedisCOWChunkMetadataTest, MutationFailsClosedOnMalformedOrWrongTypeStoredHead) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("cow-head-mutation-malformed");
  redis::RedisKey key(config.db, volume_name);
  const ChunkID chunk_id(42);
  const auto head_key = key.PrivateChunkIndex(ChunkType::kCow, "head:" + std::to_string(chunk_id.Value()));
  sw::redis::Redis redis(ConnectionOptions(config));
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, key);
  const cow::COWChunkHead expected{cow::COWChunkRevision(1), 1024};
  const cow::COWChunkHead replacement{cow::COWChunkRevision(2), 1024};
  utils::Status malformed_cas;
  utils::Status malformed_erase;
  utils::Status wrong_type_cas;
  utils::Status wrong_type_erase;

  redis.set(head_key, "not-a-cow-head");
  swordfs::test::RunInTestFiber([&] {
    malformed_cas = metadata.CompareExchangeHead(chunk_id, expected, replacement);
    malformed_erase = metadata.EraseHead(chunk_id, expected);
  });

  redis.del(head_key);
  redis.hset(head_key, "unexpected", "hash");
  swordfs::test::RunInTestFiber([&] {
    wrong_type_cas = metadata.CompareExchangeHead(chunk_id, expected, replacement);
    wrong_type_erase = metadata.EraseHead(chunk_id, expected);
  });

  backend->Shutdown();
  redis.del(head_key);

  EXPECT_EQ(malformed_cas.ToErrno(), EIO);
  EXPECT_EQ(malformed_erase.ToErrno(), EIO);
  EXPECT_EQ(wrong_type_cas.ToErrno(), EIO);
  EXPECT_EQ(wrong_type_erase.ToErrno(), EIO);
}

TEST(RedisCOWChunkMetadataTest, RejectsInvalidTypedHeadOperationsAndUnsafeSameRevisionGrowth) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, redis::RedisKey(0, "typed-validation"));
  const ChunkID chunk_id(1);
  cow::COWChunkRevision invalid_revision;
  const cow::COWChunkRevision revision(1);
  const cow::COWChunkHead head{revision, 1024};
  cow::COWChunkHead loaded;
  utils::Status invalid_id;
  utils::Status out_of_range_id;
  utils::Status null_revision;
  utils::Status invalid_get;
  utils::Status null_get;
  utils::Status invalid_head;
  utils::Status invalid_cas_id;
  utils::Status out_of_range_cas_id;
  utils::Status backward_revision;
  utils::Status invalid_expected_head;
  utils::Status out_of_range_revision;
  utils::Status invalid_growth;
  utils::Status invalid_erase;
  utils::Status invalid_erase_head;

  swordfs::test::RunInTestFiber([&] {
    invalid_id = metadata.AllocateRevision(kInvalidChunkID, &invalid_revision);
    out_of_range_id = metadata.AllocateRevision(ChunkID(kMaxChunkIDValue + 1), &invalid_revision);
    null_revision = metadata.AllocateRevision(chunk_id, nullptr);
    invalid_get = metadata.GetHead(kInvalidChunkID, &loaded);
    null_get = metadata.GetHead(chunk_id, nullptr);
    invalid_cas_id = metadata.CompareExchangeHead(kInvalidChunkID, std::nullopt, head);
    out_of_range_cas_id = metadata.CompareExchangeHead(ChunkID(kMaxChunkIDValue + 1), std::nullopt, head);
    invalid_head = metadata.CompareExchangeHead(chunk_id, std::nullopt, cow::COWChunkHead{});
    backward_revision = metadata.CompareExchangeHead(chunk_id, cow::COWChunkHead{cow::COWChunkRevision(2), 1},
                                                     cow::COWChunkHead{cow::COWChunkRevision(1), 1});
    invalid_expected_head = metadata.CompareExchangeHead(chunk_id, cow::COWChunkHead{}, head);
    out_of_range_revision = metadata.CompareExchangeHead(
        chunk_id, head, cow::COWChunkHead{cow::COWChunkRevision(cow::kMaxCOWChunkRevisionValue + 1), 1});
    invalid_growth = metadata.CompareExchangeHead(chunk_id, head, cow::COWChunkHead{revision, head.size + 1});
    invalid_erase = metadata.EraseHead(kInvalidChunkID, head);
    invalid_erase_head = metadata.EraseHead(chunk_id, cow::COWChunkHead{});
  });
  backend->Shutdown();

  EXPECT_EQ(invalid_id.ToErrno(), EINVAL);
  EXPECT_EQ(out_of_range_id.ToErrno(), EINVAL);
  EXPECT_EQ(null_revision.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_get.ToErrno(), EINVAL);
  EXPECT_EQ(null_get.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_cas_id.ToErrno(), EINVAL);
  EXPECT_EQ(out_of_range_cas_id.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_head.ToErrno(), EINVAL);
  EXPECT_EQ(backward_revision.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_expected_head.ToErrno(), EINVAL);
  EXPECT_EQ(out_of_range_revision.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_growth.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_erase.ToErrno(), EINVAL);
  EXPECT_EQ(invalid_erase_head.ToErrno(), EINVAL);

  const cow::COWChunkHead same_head{revision, 1024};
  const cow::COWChunkHead different_revision{cow::COWChunkRevision(2), 1024};
  const cow::COWChunkHead different_size{revision, 512};
  EXPECT_EQ(head, same_head);
  EXPECT_NE(head, different_revision);
  EXPECT_NE(head, different_size);
}

#ifndef NDEBUG
TEST(RedisCOWChunkMetadataTest, RuntimeAllocationRejectsThreadCaller) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisCOWChunkMetadata metadata(backend, redis::RedisKey(0, "domain"));
  ChunkID id;

  EXPECT_DEATH(
      { (void)metadata.AllocateChunkID(&id); }, "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
  backend->Shutdown();
}
#endif

}  // namespace
}  // namespace swordfs::metadata
