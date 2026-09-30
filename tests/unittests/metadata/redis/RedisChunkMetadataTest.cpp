// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <cerrno>
#include <memory>

#include "FiberTest.hpp"
#include "metadata/ChunkMetadata.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisCOWChunkMetadata.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClientFaultServer.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisTestUtils.hpp"

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
