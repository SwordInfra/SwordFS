// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClientFaultServer.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisOwnerLease.hpp"

namespace swordfs::metadata {
namespace {

using namespace std::chrono_literals;

TEST(RedisOwnerLeaseTest, RejectsTimingThatCannotRenewBeforeLocalCutoff) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";  // No Redis request is needed for validation.
  RedisOwnerLease::Timing timing;
  timing.renew_interval = 44s;
  RedisOwnerLease lease(config, "invalid-timing", timing);
  EXPECT_EQ(lease.Register().ToErrno(), EINVAL);
  EXPECT_FALSE(lease.CanGrant(1));
}

TEST(RedisOwnerLeaseTest, RejectsHugeHeartbeatIntervalWithoutOverflow) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  RedisOwnerLease::Timing timing;
  timing.renew_interval = std::chrono::milliseconds::max();
  RedisOwnerLease lease(config, "invalid-huge-interval", timing);
  EXPECT_EQ(lease.Register().ToErrno(), EINVAL);
  EXPECT_FALSE(lease.CanGrant(1));
}

TEST(RedisOwnerLeaseTest, RejectsEachInvalidLeaseTimingBoundary) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";  // Validation fails before contacting Redis.
  using Timing = RedisOwnerLease::Timing;
  const std::vector<Timing> invalid{
      {.ttl = 0ms},
      {.ttl = 25h},
      {.safety_margin = 0ms},
      {.ttl = 5s, .safety_margin = 6s},
      {.renew_interval = 0ms},
      {.io_timeout = 0ms},
      {.io_timeout = 6s},
      {.ttl = 6s, .safety_margin = 2s, .io_timeout = 5s},
      {.renew_interval = 40s},
  };
  for (const auto &timing : invalid) {
    RedisOwnerLease lease(config, "invalid-timing-boundary", timing);
    EXPECT_EQ(lease.Register().ToErrno(), EINVAL);
    EXPECT_FALSE(lease.CanGrant(1));
  }
}

TEST(RedisOwnerLeaseTest, StalledRedisIoFailsClosedWithinPerCommandBudget) {
  ScriptedRedisServer server(RedisFaultScenario::kOwnerStalledReply);
  auto config = ScriptedConfig(server);
  RedisOwnerLease::Timing timing;
  timing.io_timeout = 300ms;
  RedisOwnerLease lease(config, "stalled-owner-io", timing);

  const auto start = std::chrono::steady_clock::now();
  const auto status = lease.Register();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(lease.generation(), 0U);
  EXPECT_FALSE(lease.CanGrant(1));
  // A silent server cannot cause an unbounded *individual I/O wait*.
  // This is deliberately not an end-to-end deadline for multi-command
  // successful registration or a guarantee about OS scheduling stalls.
  EXPECT_LT(elapsed, 1500ms);
  server.Wait();
  EXPECT_EQ(server.connection_count(), 1);
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisOwnerLeaseTest, RedisRenewalFaultCannotExtendConfirmedOwnerAuthority) {
  for (const auto scenario :
       {RedisFaultScenario::kOwnerLifecycleRenewError, RedisFaultScenario::kOwnerLifecycleRenewLostReply}) {
    ScriptedRedisServer server(scenario);
    {
      RedisOwnerLease lease(ScriptedConfig(server), "faulted-owner");
      ASSERT_TRUE(lease.Register().ok());
      const auto generation = lease.generation();
      ASSERT_TRUE(lease.CanGrant(generation));
      const auto status = lease.Renew();
      EXPECT_FALSE(status.ok());
      if (scenario == RedisFaultScenario::kOwnerLifecycleRenewLostReply) {
        // An unconfirmed PEXPIRE may have renewed the Redis key. It is not
        // proof of owner validity past the last confirmed local cutoff.
        EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
        EXPECT_TRUE(lease.CanGrant(generation));
      } else {
        EXPECT_FALSE(status.IsOutcomeUnknown());
        EXPECT_FALSE(lease.CanGrant(generation));
      }
      EXPECT_TRUE(lease.Revoke().ok());
      EXPECT_FALSE(lease.CanGrant(generation));
    }
    server.Wait();
    EXPECT_TRUE(server.error().empty()) << server.error();
  }
}

TEST(RedisOwnerLeaseTest, LostRenewalReplyAfterLocalCutoffIsTerminal) {
  ScriptedRedisServer server(RedisFaultScenario::kOwnerLifecycleRenewLostReply);
  {
    auto clock = [&]() -> std::chrono::nanoseconds {
      // Only the scripted PEXPIRE marks its mutation as applied. A late
      // acknowledgement cannot rearm the same owner even if Redis renewed.
      return server.mutation_applied() ? 146s : 100s;
    };
    RedisOwnerLease lease(ScriptedConfig(server), "late-lost-renew", {}, clock);
    ASSERT_TRUE(lease.Register().ok());
    const auto generation = lease.generation();
    EXPECT_FALSE(lease.Renew().ok());
    EXPECT_TRUE(server.mutation_applied());
    EXPECT_FALSE(lease.CanGrant(generation));
    EXPECT_TRUE(lease.Revoke().ok());
  }
  server.Wait();
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisOwnerLeaseTest, FailedRevokeCannotRestoreOrRearmLease) {
  ScriptedRedisServer server(RedisFaultScenario::kOwnerLifecycleRevokeError);
  {
    RedisOwnerLease lease(ScriptedConfig(server), "faulted-revoke");
    ASSERT_TRUE(lease.Register().ok());
    const auto generation = lease.generation();
    EXPECT_FALSE(lease.Revoke().ok());
    EXPECT_FALSE(lease.CanGrant(generation));
    EXPECT_TRUE(lease.Revoke().ok());  // Idempotent; expiry remains fallback.
  }
  server.Wait();
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisOwnerLeaseTest, OverflowingRenewalDispatchFailsClosedWithoutRedisMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-renew-overflow");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  const auto max_ns = std::numeric_limits<int64_t>::max();
  std::atomic<int64_t> now_ns{max_ns - std::chrono::duration_cast<std::chrono::nanoseconds>(65s).count()};
  RedisOwnerLease lease(config, volume_name, {}, [&] { return std::chrono::nanoseconds(now_ns.load()); });
  ASSERT_TRUE(lease.Register().ok());
  const auto generation = lease.generation();
  ASSERT_TRUE(lease.CanGrant(generation));
  now_ns.store(max_ns - std::chrono::duration_cast<std::chrono::nanoseconds>(25s).count());
  EXPECT_FALSE(lease.Renew().ok());
  EXPECT_FALSE(lease.CanGrant(generation));
  EXPECT_TRUE(lease.Revoke().ok());
}

TEST(RedisOwnerLeaseTest, OverflowingRegistrationClockDoesNotCreateLease) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-register-overflow");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  const auto clock = []() -> std::chrono::nanoseconds {
    return std::chrono::nanoseconds(std::numeric_limits<int64_t>::max()) - 1s;
  };
  RedisOwnerLease lease(config, volume_name, {}, clock);
  EXPECT_FALSE(lease.Register().ok());
  EXPECT_EQ(lease.generation(), 0U);
  EXPECT_EQ(redis.exists(key.OwnerLease(1)), 0);
}

TEST(RedisOwnerLeaseTest, InvalidGenerationCounterTypeFailsClosed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-counter-wrongtype");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.lpush(key.OwnerGeneration(), "not-a-string-counter");
  RedisOwnerLease lease(config, volume_name);
  EXPECT_FALSE(lease.Register().ok());
  EXPECT_EQ(lease.generation(), 0U);
  EXPECT_EQ(redis.exists(key.OwnerLease(1)), 0);
}

TEST(RedisOwnerLeaseTest, FormatQueueFailureCannotPublishGenerationCounter) {
  ScriptedRedisServer server(RedisFaultScenario::kFormatQueueRejected);
  {
    RedisMetaOps ops(ScriptedConfig(server), "owner-failed-format");
    ASSERT_TRUE(ops.Initialize().ok());
    SwordFsVolume volume;
    volume.name = "owner-failed-format";
    volume.chunk_size = 4096;
    // A Redis error when queuing any format key must prevent the atomic
    // commit, including owner-generation initialization.
    EXPECT_FALSE(ops.FormatVolume(volume).ok());
    EXPECT_FALSE(server.exec_received());
  }
  server.Wait();
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisOwnerLeaseTest, FormattingCreatesPersistentGenerationCounter) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-format");
  const redis::RedisKey key(config.db, volume_name);
  RedisMetaOps ops(config, volume_name);
  SwordFsVolume volume;
  volume.name = volume_name;
  volume.chunk_size = 4096;
  ASSERT_TRUE(ops.Initialize().ok());
  ASSERT_TRUE(ops.FormatVolume(volume).ok());

  sw::redis::Redis redis(ConnectionOptions(config));
  EXPECT_EQ(redis.get(key.OwnerGeneration()), "0");
  EXPECT_EQ(redis.pttl(key.OwnerGeneration()), -1);
}

TEST(RedisOwnerLeaseTest, UniqueGenerationRenewalAndExactRevoke) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-lifecycle");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  std::atomic<int64_t> now_ns{100000000000LL};
  auto clock = [&] { return std::chrono::nanoseconds(now_ns.load()); };
  RedisOwnerLease::Timing timing{.ttl = 60s, .renew_interval = 10s, .safety_margin = 15s, .io_timeout = 5s};
  RedisOwnerLease first(config, volume_name, timing, clock);
  ASSERT_TRUE(first.Register().ok());
  const auto first_generation = first.generation();
  ASSERT_GT(first_generation, 0U);
  EXPECT_TRUE(first.CanGrant(first_generation));
  EXPECT_FALSE(first.CanGrant(0));  // An empty generation can never authorize a grant.
  EXPECT_FALSE(first.CanGrant(first_generation + 1));
  EXPECT_GT(redis.pttl(key.OwnerLease(first_generation)), 0);

  now_ns.store(111000000000LL);
  ASSERT_TRUE(first.Renew().ok());
  now_ns.store(145000000000LL);
  EXPECT_TRUE(first.CanGrant(first_generation));
  now_ns.store(156000000000LL);
  EXPECT_FALSE(first.CanGrant(first_generation));
  EXPECT_FALSE(first.Renew().ok());  // Expired local confidence is terminal.

  RedisOwnerLease second(config, volume_name, timing, clock);
  ASSERT_TRUE(second.Register().ok());
  ASSERT_GT(second.generation(), first_generation);
  ASSERT_TRUE(first.Revoke().ok());
  EXPECT_GT(redis.pttl(key.OwnerLease(second.generation())), 0);
  EXPECT_TRUE(second.CanGrant(second.generation()));
  ASSERT_TRUE(second.Revoke().ok());
  EXPECT_EQ(redis.exists(key.OwnerLease(second.generation())), 0);
  EXPECT_FALSE(second.CanGrant(second.generation()));
}

TEST(RedisOwnerLeaseTest, RejectsMissingCorruptNegativeAndOverflowCounters) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-invalid");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  for (const std::string &value :
       std::vector<std::string>{"", "garbage", "-1", "00", "+1", "1garbage", "18446744073709551615",
                                std::to_string(std::numeric_limits<int64_t>::max())}) {
    redis.del(key.OwnerGeneration());
    if (!value.empty()) {
      redis.set(key.OwnerGeneration(), value);
    }
    RedisOwnerLease lease(config, volume_name);
    EXPECT_FALSE(lease.Register().ok()) << "counter=" << value;
    EXPECT_EQ(lease.generation(), 0U);
    EXPECT_EQ(redis.exists(key.OwnerLease(1)), 0);
  }
}

TEST(RedisOwnerLeaseTest, RevokeOverlappingRegistrationNeverReactivatesOrLeaksHeartbeat) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-register-revoke-race");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  std::binary_semaphore registration_in_flight{0};
  std::binary_semaphore finish_registration{0};
  std::atomic<bool> blocked_once{false};
  auto clock = [&]() -> std::chrono::nanoseconds {
    if (!blocked_once.exchange(true)) {
      registration_in_flight.release();
      finish_registration.acquire();
    }
    return 100s;
  };

  RedisOwnerLease lease(config, volume_name, {}, clock);
  auto register_result = std::async(std::launch::async, [&] { return lease.Register(); });
  // This barrier places registration after generation allocation, but before
  // it can publish an active heartbeat. Revoke must not pass that transition.
  const bool reached = registration_in_flight.try_acquire_for(5s);
  if (!reached) {
    finish_registration.release();
    FAIL() << "registration did not reach the controlled clock boundary";
    return;
  }
  auto revoke_result = std::async(std::launch::async, [&] { return lease.Revoke(); });
  const auto before_release = revoke_result.wait_for(100ms);
  finish_registration.release();

  EXPECT_EQ(before_release, std::future_status::timeout);
  EXPECT_TRUE(register_result.get().ok());
  EXPECT_TRUE(revoke_result.get().ok());
  const auto generation = lease.generation();
  ASSERT_GT(generation, 0U);
  EXPECT_FALSE(lease.CanGrant(generation));
  EXPECT_EQ(redis.exists(key.OwnerLease(generation)), 0);
}

TEST(RedisOwnerLeaseTest, ServerExpiryCannotBeRenewedOrResurrected) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-expired");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");
  RedisOwnerLease lease(config, volume_name);
  ASSERT_TRUE(lease.Register().ok());
  const auto generation = lease.generation();
  redis.del(key.OwnerLease(generation));  // Authoritative expiry/revocation.
  EXPECT_FALSE(lease.Renew().ok());
  EXPECT_FALSE(lease.CanGrant(generation));
  EXPECT_EQ(redis.exists(key.OwnerLease(generation)), 0);
}

TEST(RedisOwnerLeaseTest, HeartbeatObservesRemoteRevocationWithoutUnmountOrRearm) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-heartbeat-revoke");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  RedisOwnerLease::Timing timing;
  timing.renew_interval = 20ms;
  RedisOwnerLease lease(config, volume_name, timing);
  ASSERT_TRUE(lease.Register().ok());
  const auto generation = lease.generation();
  ASSERT_TRUE(lease.CanGrant(generation));

  // Wait for the semantic transition to terminal, rather than sleeping for
  // an assumed number of heartbeat intervals.
  ASSERT_EQ(redis.del(key.OwnerLease(generation)), 1);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (lease.CanGrant(generation) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_FALSE(lease.CanGrant(generation));
  EXPECT_FALSE(lease.Renew().ok());
  EXPECT_EQ(redis.exists(key.OwnerLease(generation)), 0);
  EXPECT_TRUE(lease.Revoke().ok());
}

TEST(RedisOwnerLeaseTest, InvalidRenewalDispatchClockFailsClosed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-renew-clock-dispatch");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  std::atomic<bool> injecting_dispatch_failure{false};
  std::atomic<bool> admission_sample_observed{false};
  const auto clock = [&]() -> std::chrono::nanoseconds {
    if (!injecting_dispatch_failure.load()) {
      return 100s;
    }
    // Admission is still valid, but the subsequent dispatch-time sample is
    // invalid. No PEXPIRE or new local cutoff may be published.
    return admission_sample_observed.exchange(true) ? -1ns : 100s;
  };

  RedisOwnerLease lease(config, volume_name, {}, clock);
  ASSERT_TRUE(lease.Register().ok());
  const auto generation = lease.generation();
  const auto ttl_before = redis.pttl(key.OwnerLease(generation));
  injecting_dispatch_failure.store(true);
  EXPECT_FALSE(lease.Renew().ok());
  EXPECT_TRUE(admission_sample_observed.load());
  EXPECT_FALSE(lease.CanGrant(generation));
  EXPECT_GT(redis.pttl(key.OwnerLease(generation)), 0);
  EXPECT_LE(redis.pttl(key.OwnerLease(generation)), ttl_before);
  EXPECT_TRUE(lease.Revoke().ok());
}

TEST(RedisOwnerLeaseTest, ExistingGenerationLeaseCannotBeAdoptedOrRegisteredAgain) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-collision");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");
  redis.set(key.OwnerLease(1), "unrelated-owner", std::chrono::seconds(60));

  RedisOwnerLease lease(config, volume_name);
  EXPECT_FALSE(lease.Register().ok());
  EXPECT_EQ(lease.generation(), 0U);
  EXPECT_FALSE(lease.CanGrant(1));
  EXPECT_FALSE(lease.Register().ok());  // An ambiguous/colliding epoch is terminal.
  EXPECT_EQ(redis.get(key.OwnerLease(1)), "unrelated-owner");
  EXPECT_EQ(redis.get(key.OwnerGeneration()), "1");
}

TEST(RedisOwnerLeaseTest, RegistrationRejectsLateConfirmationWithoutReusingGeneration) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-late-register");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  // Advance monotonic time beyond the cutoff before confirmation is
  // observed. The Redis key may exist, but cannot become a local grant.
  const auto clock = [calls = 0]() mutable -> std::chrono::nanoseconds {
    return ++calls == 1 ? std::chrono::seconds(100) : std::chrono::seconds(150);
  };
  RedisOwnerLease lease(config, volume_name, {}, clock);
  EXPECT_FALSE(lease.Register().ok());
  EXPECT_EQ(lease.generation(), 0U);
  EXPECT_FALSE(lease.CanGrant(1));
  EXPECT_EQ(redis.get(key.OwnerGeneration()), "1");
  EXPECT_GT(redis.pttl(key.OwnerLease(1)), 0);
  EXPECT_FALSE(lease.Register().ok());
}

TEST(RedisOwnerLeaseTest, InvalidClockFailsClosedAtRegistrationAndRenewal) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-invalid-clock");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  RedisOwnerLease negative(config, volume_name, {}, [] { return std::chrono::nanoseconds(-1); });
  EXPECT_FALSE(negative.Register().ok());
  EXPECT_FALSE(negative.Register().ok());

  std::atomic<int64_t> now_ns{std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(100)).count()};
  auto clock = [&] { return std::chrono::nanoseconds(now_ns.load()); };
  RedisOwnerLease active(config, volume_name, {}, clock);
  ASSERT_TRUE(active.Register().ok());
  const auto generation = active.generation();
  now_ns.store(-1);
  // Grant admission itself must reject an invalid clock, without waiting for
  // a heartbeat or explicit renewal attempt.
  EXPECT_FALSE(active.CanGrant(generation));
  now_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(101s).count());
  EXPECT_FALSE(active.CanGrant(generation));  // Recovery cannot rearm a terminal owner.
  EXPECT_FALSE(active.Renew().ok());
  EXPECT_TRUE(active.Revoke().ok());
}

TEST(RedisOwnerLeaseTest, LateRenewalConfirmationCannotExtendLocalConfidence) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = UniqueRedisName("owner-late-renew");
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.OwnerGeneration(), "0");

  // Observe the authoritative PEXPIRE, not the number of times the
  // implementation happens to sample its clock. Once Redis has renewed
  // the TTL, simulate a local clock that has passed the old cutoff.
  std::atomic<bool> observing_renewal{false};
  std::atomic<bool> confirmed_renewal_observed{false};
  uint64_t generation = 0;
  const auto clock = [&]() -> std::chrono::nanoseconds {
    if (observing_renewal.load() && redis.pttl(key.OwnerLease(generation)) > 30000) {
      // The first local post-ack admission remains inside the old cutoff,
      // but the subsequent final confirmation sample has crossed it.
      return confirmed_renewal_observed.exchange(true) ? 146s : 100s;
    }
    return 100s;
  };
  RedisOwnerLease lease(config, volume_name, {}, clock);
  ASSERT_TRUE(lease.Register().ok());
  generation = lease.generation();
  ASSERT_TRUE(redis.pexpire(key.OwnerLease(generation), 10000));
  observing_renewal.store(true);
  EXPECT_FALSE(lease.Renew().ok());
  EXPECT_TRUE(confirmed_renewal_observed.load());
  EXPECT_FALSE(lease.CanGrant(generation));
  EXPECT_TRUE(lease.Revoke().ok());
  EXPECT_EQ(redis.exists(key.OwnerLease(generation)), 0);
}

}  // namespace
}  // namespace swordfs::metadata
