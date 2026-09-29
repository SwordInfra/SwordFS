// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <cerrno>
#include <memory>

#include "FiberTest.hpp"
#include "metadata/IPrivateMetadata.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClientFaultServer.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisPrivateMetadataStore.hpp"
#include "metadata/redis/RedisTestUtils.hpp"

namespace swordfs::metadata {
namespace {

using COWSequenceA = PrivateSequenceTag<ChunkType::kCow, 21>;
using COWSequenceB = PrivateSequenceTag<ChunkType::kCow, 22>;
using SliceSequenceA = PrivateSequenceTag<ChunkType::kChunkSlice, 21>;

TEST(RedisPrivateMetadataStoreTest, AllocatesIndependentMechanismAndTagSequences) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("private-sequence");
  redis::RedisKey key(config.db, volume_name);
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisPrivateMetadataStore cow(backend, key, ChunkType::kCow);
  RedisPrivateMetadataStore chunk_slice(backend, key, ChunkType::kChunkSlice);

  uint64_t first = 0;
  uint64_t second = 0;
  uint64_t other_tag = 0;
  uint64_t other_mechanism = 0;
  utils::Status first_status;
  utils::Status second_status;
  utils::Status tag_status;
  utils::Status mechanism_status;
  swordfs::test::RunInTestFiber([&] {
    first_status = cow.AllocateSequence(COWSequenceA{}, &first);
    second_status = cow.AllocateSequence(COWSequenceA{}, &second);
    tag_status = cow.AllocateSequence(COWSequenceB{}, &other_tag);
    mechanism_status = chunk_slice.AllocateSequence(SliceSequenceA{}, &other_mechanism);
  });

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(key.PrivateSequence(ChunkType::kCow, COWSequenceA::kStableId));
  cleanup.del(key.PrivateSequence(ChunkType::kCow, COWSequenceB::kStableId));
  cleanup.del(key.PrivateSequence(ChunkType::kChunkSlice, SliceSequenceA::kStableId));
  backend->Shutdown();

  ASSERT_TRUE(first_status.ok()) << first_status.message();
  ASSERT_TRUE(second_status.ok()) << second_status.message();
  ASSERT_TRUE(tag_status.ok()) << tag_status.message();
  ASSERT_TRUE(mechanism_status.ok()) << mechanism_status.message();
  EXPECT_EQ(first, 1U);
  EXPECT_EQ(second, 2U);
  EXPECT_EQ(other_tag, 1U);
  EXPECT_EQ(other_mechanism, 1U);
  EXPECT_EQ(cow.AllocateSequence(SliceSequenceA{}, &first).ToErrno(), EINVAL);
}

TEST(RedisPrivateMetadataStoreTest, RejectsNegativeCounterWithoutMutatingIt) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("private-sequence-range");
  redis::RedisKey key(config.db, volume_name);
  const auto sequence_key = key.PrivateSequence(ChunkType::kCow, COWSequenceA::kStableId);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(sequence_key, "-1");

  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisPrivateMetadataStore store(backend, key, ChunkType::kCow);
  uint64_t first = 0;
  uint64_t second = 0;
  utils::Status first_status;
  utils::Status second_status;
  swordfs::test::RunInTestFiber([&] {
    first_status = store.AllocateSequence(COWSequenceA{}, &first);
    second_status = store.AllocateSequence(COWSequenceA{}, &second);
  });
  const auto persisted = redis.get(sequence_key);
  redis.del(sequence_key);
  backend->Shutdown();

  EXPECT_EQ(first_status.ToErrno(), EIO);
  EXPECT_EQ(second_status.ToErrno(), EIO);
  EXPECT_EQ(first, 0U);
  EXPECT_EQ(second, 0U);
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(*persisted, "-1");
}

TEST(RedisPrivateMetadataStoreTest, ExhaustionFailsClosedWithoutAdvancingCounter) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("private-sequence-exhaustion");
  redis::RedisKey key(config.db, volume_name);
  const auto sequence_key = key.PrivateSequence(ChunkType::kCow, COWSequenceA::kStableId);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(sequence_key, std::to_string(kMaxPrivateSequenceValue));

  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisPrivateMetadataStore store(backend, key, ChunkType::kCow);
  uint64_t first = 0;
  uint64_t second = 0;
  utils::Status first_status;
  utils::Status second_status;
  swordfs::test::RunInTestFiber([&] {
    first_status = store.AllocateSequence(COWSequenceA{}, &first);
    second_status = store.AllocateSequence(COWSequenceA{}, &second);
  });
  const auto persisted = redis.get(sequence_key);
  redis.del(sequence_key);
  backend->Shutdown();

  EXPECT_EQ(first_status.ToErrno(), EIO);
  EXPECT_EQ(second_status.ToErrno(), EIO);
  EXPECT_EQ(first, 0U);
  EXPECT_EQ(second, 0U);
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(*persisted, std::to_string(kMaxPrivateSequenceValue));
}

TEST(RedisPrivateMetadataStoreTest, AmbiguousIncrOutcomePropagatesWithoutReplay) {
  for (const auto scenario :
       {RedisFaultScenario::kIncrProtocolFailureApplied, RedisFaultScenario::kIncrDisconnectNotApplied}) {
    ScriptedRedisServer server(scenario);
    auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
    redis::RedisKey key(0, "private-sequence-fault");
    RedisPrivateMetadataStore store(backend, key, ChunkType::kCow);
    uint64_t value = 0;
    utils::Status status;

    swordfs::test::RunInTestFiber([&] { status = store.AllocateSequence(COWSequenceA{}, &value); });
    server.Wait();
    backend->Shutdown();

    EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
    EXPECT_TRUE(server.error().empty()) << server.error();
    EXPECT_EQ(server.mutation_applied(), scenario == RedisFaultScenario::kIncrProtocolFailureApplied);
  }
}

TEST(RedisPrivateMetadataStoreTest, AmbiguousAppliedAllocationLeavesGapAndRetryGetsFreshValue) {
  ScriptedRedisServer server(RedisFaultScenario::kIncrProtocolFailureAppliedThenFreshValue);
  auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
  redis::RedisKey key(0, "private-sequence-fresh-retry");
  RedisPrivateMetadataStore store(backend, key, ChunkType::kCow);
  uint64_t lost = 0;
  uint64_t fresh = 0;
  utils::Status lost_status;
  utils::Status fresh_status;

  swordfs::test::RunInTestFiber([&] {
    lost_status = store.AllocateSequence(COWSequenceA{}, &lost);
    fresh_status = store.AllocateSequence(COWSequenceA{}, &fresh);
  });
  server.Wait();
  backend->Shutdown();

  EXPECT_TRUE(lost_status.IsOutcomeUnknown()) << lost_status.message();
  ASSERT_TRUE(fresh_status.ok()) << fresh_status.message();
  EXPECT_EQ(lost, 0U);
  EXPECT_EQ(fresh, 2U);
  EXPECT_TRUE(server.mutation_applied());
  EXPECT_EQ(server.connection_count(), 2);
  EXPECT_TRUE(server.error().empty()) << server.error();
}

#ifndef NDEBUG
TEST(RedisPrivateMetadataStoreTest, RuntimeAllocationRejectsThreadCaller) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisPrivateMetadataStore store(backend, redis::RedisKey(0, "domain"), ChunkType::kCow);
  uint64_t value = 0;

  EXPECT_DEATH(
      { (void)store.AllocateSequence(COWSequenceA{}, &value); },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
  backend->Shutdown();
}
#endif

}  // namespace
}  // namespace swordfs::metadata
