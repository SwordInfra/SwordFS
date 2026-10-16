// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <fcntl.h>
#include <folly/CancellationToken.h>
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <sys/file.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "FiberTest.hpp"
#include "fuse/FuseAdvisoryLocks.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisLockAuthority.hpp"
#include "metadata/redis/RedisLockFaultProxy.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisOwnerLease.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/FiberRuntime.hpp"

namespace swordfs::metadata {
namespace {

class RedisLockAuthorityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!ParseTestConfig(&config_)) {
      GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
    }
    volume_ = UniqueRedisName("lock-authority");
    const redis::RedisKey key(config_.db, volume_);
    sw::redis::Redis redis(ConnectionOptions(config_));
    redis.set(key.OwnerGeneration(), "0");
    first_lease_ = std::make_unique<RedisOwnerLease>(config_, volume_);
    second_lease_ = std::make_unique<RedisOwnerLease>(config_, volume_);
    ASSERT_TRUE(first_lease_->Register().ok());
    ASSERT_TRUE(second_lease_->Register().ok());
    first_ = std::make_unique<RedisLockAuthority>(config_, volume_, *first_lease_);
    second_ = std::make_unique<RedisLockAuthority>(config_, volume_, *second_lease_);
  }

  void TearDown() override {
    second_.reset();
    first_.reset();
    if (second_lease_) {
      EXPECT_TRUE(second_lease_->Revoke().ok());
    }
    if (first_lease_) {
      EXPECT_TRUE(first_lease_->Revoke().ok());
    }
  }

  RedisMetaConfig config_;
  std::string volume_;
  std::unique_ptr<RedisOwnerLease> first_lease_;
  std::unique_ptr<RedisOwnerLease> second_lease_;
  std::unique_ptr<RedisLockAuthority> first_;
  std::unique_ptr<RedisLockAuthority> second_;
};

using PosixType = RedisLockAuthority::PosixType;
using PosixRange = RedisLockAuthority::PosixRange;
using FlockType = RedisLockAuthority::FlockType;

// The blocking flock path must not reject uncontended requests just because
// they omit LOCK_NB. The first attempt still uses the existing Redis authority.
FIBER_TEST_F(RedisLockAuthorityTest, BlockingFlockAcquiresWithoutContention) {
  fuse::FuseAdvisoryLocks locks(*first_, swordfs::test::BlockingTestExecutor());
  EXPECT_TRUE(locks.Flock(771, 100, LOCK_EX).ok());
  EXPECT_TRUE(locks.Flock(771, 100, LOCK_UN).ok());
}

FIBER_TEST_F(RedisLockAuthorityTest, PreCancelledBlockingRequestsDoNotAcquireLocks) {
  fuse::FuseAdvisoryLocks locks(*second_, swordfs::test::BlockingTestExecutor());
  folly::CancellationSource source;
  source.requestCancellation();
  EXPECT_EQ(locks.Flock(778, 202, LOCK_EX, source.getToken()).ToErrno(), EINTR);
  struct flock request{};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 0;
  request.l_len = 0;
  EXPECT_EQ(locks.SetPosix(779, 202, request, true, source.getToken()).ToErrno(), EINTR);
  // Redis mutation is thread-domain work even when the lock caller is a Fiber.
  EXPECT_TRUE(swordfs::test::BlockingTestExecutor()
                  .RunFromFiber([&] { return first_->SetFlock(778, 303, FlockType::kExclusive); })
                  .ok());
  EXPECT_TRUE(swordfs::test::BlockingTestExecutor()
                  .RunFromFiber([&] { return first_->SetPosix(779, 303, {0, 0, PosixType::kWrite}); })
                  .ok());
}

// A waiter must not be admitted unless the driver runtime can schedule
// its read-only probe coordinator; failure leaves no phantom waiter behind.
FIBER_TEST_F(RedisLockAuthorityTest, LockWaitAdmissionRequiresDriverRuntime) {
  fuse::LockRetryScheduler scheduler(*second_, swordfs::test::BlockingTestExecutor());
  auto waiter = std::make_shared<fuse::LockRetryScheduler::Waiter>();
  waiter->inode_ino = 781;
  waiter->kernel_owner = 202;
  waiter->flock = true;
  waiter->flock_type = FlockType::kExclusive;
  EXPECT_EQ(scheduler.RegisterWaiter(waiter).ToErrno(), EIO);
  EXPECT_EQ(scheduler.RegisterWaiter(waiter).ToErrno(), EIO);
}

// Opt-in-independent, bounded Redis-backed contention cost sample. The
// baseline performs per-owner rejected TryLock calls; the grouped alternative
// makes one validated read-only Inspect per hot inode/class group. This
// measures real backend commands, rather than treating method-call counts as
// redis traffic. It is diagnostic: host contention affects timing values.
TEST_F(RedisLockAuthorityTest, GroupedReadOnlyProbesReduceRedisCommandCostUnderContention) {
  constexpr uint64_t kInode = 789;
  constexpr size_t kOwners = 16;
  constexpr size_t kRounds = 24;
  ASSERT_TRUE(first_->SetFlock(kInode, 101, FlockType::kExclusive).ok());
  sw::redis::Redis metrics(ConnectionOptions(config_));
  struct Sample {
    uint64_t redis_commands;
    double cpu_ms;
    double p95_ms;
    double p99_ms;
  };
  const auto measure = [&](bool grouped) {
    const uint64_t before = RedisInfoCounter(metrics, "stats", "total_commands_processed");
    const std::clock_t cpu_start = std::clock();
    std::vector<double> samples;
    samples.reserve(kRounds);
    for (size_t round = 0; round < kRounds; ++round) {
      const auto start = std::chrono::steady_clock::now();
      if (grouped) {
        std::vector<ILockAuthority::LockHolder> holders;
        const auto status = second_->Inspect(kInode, true, &holders);
        EXPECT_TRUE(status.ok()) << status.message();
        EXPECT_EQ(holders.size(), 1U);
      } else {
        for (size_t owner = 0; owner < kOwners; ++owner) {
          const auto status = second_->SetFlock(kInode, 200 + owner, FlockType::kExclusive);
          EXPECT_EQ(status.ToErrno(), EAGAIN);
        }
      }
      samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const double cpu_ms = static_cast<double>(std::clock() - cpu_start) * 1000 / CLOCKS_PER_SEC;
    const uint64_t after = RedisInfoCounter(metrics, "stats", "total_commands_processed");
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](size_t pct) { return samples[(samples.size() * pct + 99) / 100 - 1]; };
    return Sample{after - before, cpu_ms, percentile(95), percentile(99)};
  };
  const auto individual = measure(false);
  const auto grouped = measure(true);
  std::cout << "LOCK_CONTENTION_REDIS_COST mode=per_waiter owners=" << kOwners << " rounds=" << kRounds
            << " redis_commands=" << individual.redis_commands << " cpu_ms=" << individual.cpu_ms
            << " p95_round_ms=" << individual.p95_ms << " p99_round_ms=" << individual.p99_ms << '\n';
  std::cout << "LOCK_CONTENTION_REDIS_COST mode=grouped owners=" << kOwners << " rounds=" << kRounds
            << " redis_commands=" << grouped.redis_commands << " cpu_ms=" << grouped.cpu_ms
            << " p95_round_ms=" << grouped.p95_ms << " p99_round_ms=" << grouped.p99_ms << '\n';
  // Command efficiency is the functional cost property here; latency and CPU
  // are reported without a CI threshold because runner noise is uncontrolled.
  EXPECT_LT(grouped.redis_commands, individual.redis_commands);
  EXPECT_TRUE(first_->SetFlock(kInode, 101, FlockType::kUnlock).ok());

  // Distinct from the round-trip probe benchmark above: this samples actual
  // blocked Fiber grant completion after the remote owner releases the lock.
  // A fixed hold period is benchmark setup, NEVER evidence of waiter admission
  // or correctness; the future's completion is the semantic success barrier.
  utils::BlockingExecutor executor(1, "grant-latency-sample");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  utils::FiberRuntime runtime;
  std::vector<double> grant_ms;
  constexpr size_t kGrantSamples = 24;
  grant_ms.reserve(kGrantSamples);
  folly::CancellationSource cancellation;
  auto cleanup = folly::makeGuard([&] {
    cancellation.requestCancellation();
    runtime.Shutdown();
  });
  for (size_t trial = 0; trial < kGrantSamples; ++trial) {
    ASSERT_TRUE(first_->SetFlock(kInode + 1, 101, FlockType::kExclusive).ok());
    std::promise<utils::Status> reply;
    auto result = reply.get_future();
    ASSERT_TRUE(runtime.Submit(
        [&] { reply.set_value(locks.Flock(kInode + 1, 1000 + trial, LOCK_EX, cancellation.getToken())); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    const auto released_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(first_->SetFlock(kInode + 1, 101, FlockType::kUnlock).ok());
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(result.get().ok());
    grant_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - released_at).count());
    ASSERT_TRUE(second_->SetFlock(kInode + 1, 1000 + trial, FlockType::kUnlock).ok());
  }
  std::sort(grant_ms.begin(), grant_ms.end());
  std::cout << "LOCK_CONTENTION_GRANT_LAG samples=" << kGrantSamples
            << " p95_after_unlock_ms=" << grant_ms[(kGrantSamples * 95 + 99) / 100 - 1]
            << " p99_after_unlock_ms=" << grant_ms[(kGrantSamples * 99 + 99) / 100 - 1] << '\n';
}

TEST_F(RedisLockAuthorityTest, ConflictingExclusiveAndCompatibleSharedGrantsAcrossMounts) {
  ASSERT_TRUE(first_->SetPosix(21, 100, {0, 0, PosixType::kRead}).ok());
  EXPECT_TRUE(second_->SetPosix(21, 200, {0, 0, PosixType::kRead}).ok());
  EXPECT_EQ(second_->SetPosix(21, 200, {0, 0, PosixType::kWrite}).ToErrno(), EAGAIN);
  EXPECT_EQ(second_->SetPosix(21, 200, {8, 1, PosixType::kWrite}).ToErrno(), EAGAIN);
  EXPECT_TRUE(second_->SetPosix(21, 200, {0, 0, PosixType::kUnlock}).ok());
  EXPECT_TRUE(first_->SetPosix(21, 100, {0, 0, PosixType::kUnlock}).ok());
  EXPECT_TRUE(second_->SetPosix(21, 200, {0, 0, PosixType::kWrite}).ok());
}

// Linux flock conversion may relinquish SH before attempting EX. Retaining
// the old SH on EAGAIN makes two independent upgraders deadlock indefinitely.
TEST_F(RedisLockAuthorityTest, ConflictedSharedToExclusiveUpgradeReleasesOldSharedLock) {
  ASSERT_TRUE(first_->SetFlock(772, 101, FlockType::kShared).ok());
  ASSERT_TRUE(second_->SetFlock(772, 202, FlockType::kShared).ok());
  EXPECT_EQ(first_->SetFlock(772, 101, FlockType::kExclusive).ToErrno(), EAGAIN);
  EXPECT_TRUE(second_->SetFlock(772, 202, FlockType::kExclusive).ok());
}

TEST_F(RedisLockAuthorityTest, BlockingFlockWaitReleasesWorkerAndGrantsAfterUnlock) {
  utils::BlockingExecutor executor(1, "blocking-flock-worker");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  ASSERT_TRUE(first_->SetFlock(773, 101, FlockType::kExclusive).ok());
  utils::FiberRuntime runtime;
  std::promise<utils::Status> reply;
  auto future = reply.get_future();
  ASSERT_TRUE(runtime.Submit([&] { reply.set_value(locks.Flock(773, 202, LOCK_EX)); }));
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  // This foreground unlock shares the *single* worker with the background
  // probe. A waiting request must never occupy that worker indefinitely.
  EXPECT_TRUE(executor.RunFromThread([&] { return first_->SetFlock(773, 101, FlockType::kUnlock); }).ok());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  EXPECT_TRUE(second_->SetFlock(773, 202, FlockType::kUnlock).ok());
  runtime.Shutdown();
}

TEST_F(RedisLockAuthorityTest, BlockingPosixWaitPreservesRangeAfterRemoteUnlock) {
  utils::BlockingExecutor executor(1, "blocking-posix-worker");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  ASSERT_TRUE(first_->SetPosix(774, 101, {10, 30, PosixType::kWrite}).ok());
  utils::FiberRuntime runtime;
  std::promise<utils::Status> reply;
  auto future = reply.get_future();
  struct flock request{};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 20;
  request.l_len = 10;
  request.l_pid = 42;
  ASSERT_TRUE(runtime.Submit([&] { reply.set_value(locks.SetPosix(774, 202, request, true)); }));
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  EXPECT_TRUE(executor.RunFromThread([&] { return first_->SetPosix(774, 101, {10, 30, PosixType::kUnlock}); }).ok());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  std::optional<RedisLockAuthority::PosixConflict> conflict;
  ASSERT_TRUE(first_->GetPosix(774, 303, {20, 10, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 42);
  EXPECT_EQ(conflict->start, 20U);
  EXPECT_EQ(conflict->length, 10U);
  runtime.Shutdown();
}

TEST_F(RedisLockAuthorityTest, CancelledBlockingFlockWaitNeverGrantsOrUnmounts) {
  utils::BlockingExecutor executor(1, "blocking-flock-cancel");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  ASSERT_TRUE(first_->SetFlock(775, 101, FlockType::kExclusive).ok());
  utils::FiberRuntime runtime;
  folly::CancellationSource source;
  std::promise<utils::Status> reply;
  auto future = reply.get_future();
  ASSERT_TRUE(runtime.Submit([&] { reply.set_value(locks.Flock(775, 202, LOCK_EX, source.getToken())); }));
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  source.requestCancellation();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(future.get().ToErrno(), EINTR);
  ASSERT_TRUE(first_->SetFlock(775, 101, FlockType::kUnlock).ok());
  EXPECT_TRUE(second_->SetFlock(775, 202, FlockType::kExclusive).ok());
  runtime.Shutdown();
}

TEST_F(RedisLockAuthorityTest, CompatibleSharedWaitersAcrossMultipleEventBasesMakeProgress) {
  utils::BlockingExecutor executor(1, "lock-shared-multi-eventbase");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  ASSERT_TRUE(first_->SetFlock(777, 101, FlockType::kExclusive).ok());
  utils::FiberRuntime first_runtime;
  utils::FiberRuntime second_runtime;
  constexpr size_t kWaiterCount = 6;
  std::array<std::promise<utils::Status>, kWaiterCount> replies;
  std::array<std::future<utils::Status>, kWaiterCount> futures;
  for (size_t i = 0; i < kWaiterCount; ++i) {
    futures[i] = replies[i].get_future();
    auto &runtime = i % 2 == 0 ? first_runtime : second_runtime;
    ASSERT_TRUE(runtime.Submit([&, i] { replies[i].set_value(locks.Flock(777, 500 + i, LOCK_SH)); }));
  }
  for (auto &future : futures) {
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  }
  EXPECT_TRUE(first_->SetFlock(777, 101, FlockType::kUnlock).ok());
  for (size_t i = 0; i < kWaiterCount; ++i) {
    ASSERT_EQ(futures[i].wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(futures[i].get().ok());
    EXPECT_TRUE(second_->SetFlock(777, 500 + i, FlockType::kUnlock).ok());
  }
  first_runtime.Shutdown();
  second_runtime.Shutdown();
}

// A probe may nominate one conflicting exclusive candidate, but it cannot
// grant both. The second requester must remain waiting until the first
// committed owner explicitly unlocks its record range.
TEST_F(RedisLockAuthorityTest, CompetingExclusivePosixWaitersRemainSerialized) {
  utils::BlockingExecutor executor(1, "competing-posix-waiters");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  constexpr uint64_t kInode = 780;
  ASSERT_TRUE(first_->SetPosix(kInode, 101, {0, 0, PosixType::kWrite}).ok());
  utils::FiberRuntime first_runtime;
  utils::FiberRuntime second_runtime;
  folly::CancellationSource cancellation;
  struct flock request{};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 0;
  request.l_len = 0;
  std::promise<utils::Status> first_reply;
  std::promise<utils::Status> second_reply;
  auto first_result = first_reply.get_future();
  auto second_result = second_reply.get_future();
  auto stop_runtimes = folly::makeGuard([&] {
    cancellation.requestCancellation();
    first_runtime.Shutdown();
    second_runtime.Shutdown();
  });
  ASSERT_TRUE(first_runtime.Submit(
      [&] { first_reply.set_value(locks.SetPosix(kInode, 202, request, true, cancellation.getToken())); }));
  ASSERT_TRUE(second_runtime.Submit(
      [&] { second_reply.set_value(locks.SetPosix(kInode, 303, request, true, cancellation.getToken())); }));
  EXPECT_EQ(first_result.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  EXPECT_EQ(second_result.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  ASSERT_TRUE(first_->SetPosix(kInode, 101, {0, 0, PosixType::kUnlock}).ok());
  const bool first_won = first_result.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  const bool second_won = second_result.wait_for(first_won ? std::chrono::milliseconds(100)
                                                           : std::chrono::seconds(5)) == std::future_status::ready;
  ASSERT_NE(first_won, second_won);
  if (first_won) {
    EXPECT_TRUE(first_result.get().ok());
    EXPECT_EQ(second_result.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  } else {
    EXPECT_TRUE(second_result.get().ok());
    EXPECT_EQ(first_result.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  }
  const uint64_t winner = first_won ? 202 : 303;
  ASSERT_TRUE(second_->SetPosix(kInode, winner, {0, 0, PosixType::kUnlock}).ok());
  auto &remaining = first_won ? second_result : first_result;
  ASSERT_EQ(remaining.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(remaining.get().ok());
  ASSERT_TRUE(second_->SetPosix(kInode, first_won ? 303 : 202, {0, 0, PosixType::kUnlock}).ok());
}

// A revoked owner generation must never continue polling or grant a lock.
// Its read-only probe fails closed and releases the suspended request.
TEST_F(RedisLockAuthorityTest, BlockingWaitStopsWhenInspectCannotValidateLease) {
  utils::BlockingExecutor executor(1, "lease-revoked-waiter");
  fuse::FuseAdvisoryLocks locks(*second_, executor);
  ASSERT_TRUE(first_->SetFlock(782, 101, FlockType::kExclusive).ok());
  utils::FiberRuntime runtime;
  folly::CancellationSource cancellation;
  std::promise<utils::Status> reply;
  auto result = reply.get_future();
  auto stop_runtime = folly::makeGuard([&] {
    cancellation.requestCancellation();
    runtime.Shutdown();
  });
  ASSERT_TRUE(runtime.Submit([&] { reply.set_value(locks.Flock(782, 202, LOCK_EX, cancellation.getToken())); }));
  EXPECT_EQ(result.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  ASSERT_TRUE(second_lease_->Revoke().ok());
  ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(result.get().ToErrno(), EIO);
  EXPECT_TRUE(first_->SetFlock(782, 101, FlockType::kUnlock).ok());
}

TEST_F(RedisLockAuthorityTest, InspectRejectsMalformedStoredRangeWithoutGranting) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const std::string hash = key.PosixLock(785);
  const std::string owner = std::to_string(first_lease_->generation()) + ":101";
  redis.hset(hash, owner, "P2|W,0,100,-1");
  std::vector<ILockAuthority::LockHolder> snapshot;
  const auto status = second_->Inspect(785, false, &snapshot);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(snapshot.empty());
  redis.del(hash);
}

TEST_F(RedisLockAuthorityTest, InspectRejectsMissingOutputWithoutReadingRedis) {
  EXPECT_EQ(second_->Inspect(783, true, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(second_->Inspect(783, false, nullptr).ToErrno(), EINVAL);
}

TEST_F(RedisLockAuthorityTest, ReadOnlyInspectionDistinguishesSameOwnerAcrossGenerations) {
  ASSERT_TRUE(first_->SetFlock(776, 202, FlockType::kShared).ok());
  std::vector<ILockAuthority::LockHolder> holders;
  ASSERT_TRUE(second_->Inspect(776, true, &holders).ok());
  ASSERT_EQ(holders.size(), 1U);
  EXPECT_EQ(holders.front().kernel_owner, 202U);
  EXPECT_FALSE(holders.front().local_generation);
  EXPECT_EQ(holders.front().flock, FlockType::kShared);
  ASSERT_TRUE(first_->SetPosix(776, 202, {1, 5, PosixType::kWrite, 123}).ok());
  ASSERT_TRUE(first_->Inspect(776, false, &holders).ok());
  ASSERT_EQ(holders.size(), 1U);
  EXPECT_TRUE(holders.front().local_generation);
  ASSERT_EQ(holders.front().ranges.size(), 1U);
  EXPECT_EQ(holders.front().ranges.front().start, 1U);
  EXPECT_EQ(holders.front().ranges.front().length, 5U);
  EXPECT_EQ(holders.front().ranges.front().pid, 123);
  ASSERT_TRUE(first_lease_->Revoke().ok());
  ASSERT_TRUE(second_->Inspect(776, true, &holders).ok());
  EXPECT_TRUE(holders.empty());
  EXPECT_EQ(first_->Inspect(776, false, &holders).ToErrno(), EIO);
}

TEST_F(RedisLockAuthorityTest, ConcurrentExclusiveCandidatesNeverBothSucceed) {
  auto first = std::async(std::launch::async, [&] { return first_->SetPosix(29, 100, {0, 0, PosixType::kWrite}); });
  auto second = std::async(std::launch::async, [&] { return second_->SetPosix(29, 200, {0, 0, PosixType::kWrite}); });
  const auto first_status = first.get();
  const auto second_status = second.get();
  EXPECT_NE(first_status.ok(), second_status.ok());
  if (!first_status.ok()) {
    EXPECT_EQ(first_status.ToErrno(), EAGAIN);
  }
  if (!second_status.ok()) {
    EXPECT_EQ(second_status.ToErrno(), EAGAIN);
  }
}

TEST_F(RedisLockAuthorityTest, DisjointRangesAndSameOwnerConversionSplitAndMerge) {
  ASSERT_TRUE(first_->SetPosix(22, 100, {0, 100, PosixType::kWrite}).ok());
  EXPECT_TRUE(second_->SetPosix(22, 200, {100, 100, PosixType::kWrite}).ok());
  EXPECT_EQ(second_->SetPosix(22, 200, {99, 2, PosixType::kWrite}).ToErrno(), EAGAIN);
  EXPECT_TRUE(first_->SetPosix(22, 100, {20, 40, PosixType::kUnlock}).ok());
  EXPECT_TRUE(second_->SetPosix(22, 200, {20, 40, PosixType::kWrite}).ok());
  EXPECT_TRUE(first_->SetPosix(22, 100, {0, 20, PosixType::kRead}).ok());
  EXPECT_TRUE(second_->SetPosix(22, 200, {0, 20, PosixType::kRead}).ok());
  EXPECT_EQ(first_->SetPosix(22, 100, {0, 20, PosixType::kWrite}).ToErrno(), EAGAIN);
}

TEST_F(RedisLockAuthorityTest, GetlkReportsLiveConflictsAndIgnoresSelfAndExpiredOwner) {
  ASSERT_TRUE(first_->SetPosix(23, 100, {10, 30, PosixType::kWrite}).ok());
  std::optional<RedisLockAuthority::PosixConflict> conflict;
  ASSERT_TRUE(second_->GetPosix(23, 200, {20, 10, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->type, PosixType::kWrite);
  EXPECT_EQ(conflict->start, 10U);
  EXPECT_EQ(conflict->kernel_owner, 100U);
  ASSERT_TRUE(first_->GetPosix(23, 100, {20, 10, PosixType::kWrite}, &conflict).ok());
  EXPECT_FALSE(conflict.has_value());
  ASSERT_TRUE(first_lease_->Revoke().ok());
  ASSERT_TRUE(second_->GetPosix(23, 200, {20, 10, PosixType::kWrite}, &conflict).ok());
  EXPECT_FALSE(conflict.has_value());
  EXPECT_FALSE(first_->SetPosix(23, 100, {100, 1, PosixType::kWrite}).ok());
}

TEST_F(RedisLockAuthorityTest, GetlkPreservesHolderPidAcrossMountsAndSameOwnerSplits) {
  // PID must describe the conflicting lock segment, not the opaque FUSE owner.
  ASSERT_TRUE(first_->SetPosix(231, 0xabcdef, {10, 90, PosixType::kWrite, 1234}).ok());
  ASSERT_TRUE(first_->SetPosix(231, 0xabcdef, {40, 20, PosixType::kRead, 5678}).ok());

  std::optional<RedisLockAuthority::PosixConflict> conflict;
  ASSERT_TRUE(second_->GetPosix(231, 0xabcdef, {15, 1, PosixType::kRead}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 1234);
  EXPECT_EQ(conflict->start, 10U);
  EXPECT_EQ(conflict->length, 30U);

  ASSERT_TRUE(second_->GetPosix(231, 999, {45, 1, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 5678);
  EXPECT_EQ(conflict->type, PosixType::kRead);
  EXPECT_EQ(conflict->start, 40U);
  EXPECT_EQ(conflict->length, 20U);

  ASSERT_TRUE(first_->GetPosix(231, 0xabcdef, {15, 1, PosixType::kWrite}, &conflict).ok());
  EXPECT_FALSE(conflict.has_value());
}

TEST_F(RedisLockAuthorityTest, AdjacentSameModeRangesWithDistinctPidsPreserveTheirHolders) {
  // Adjacent ranges may merge only when both mode and diagnostic PID agree.
  ASSERT_TRUE(first_->SetPosix(234, 100, {0, 40, PosixType::kWrite, 1234}).ok());
  ASSERT_TRUE(first_->SetPosix(234, 100, {40, 60, PosixType::kWrite, 5678}).ok());

  std::optional<RedisLockAuthority::PosixConflict> conflict;
  ASSERT_TRUE(second_->GetPosix(234, 200, {10, 1, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 1234);
  EXPECT_EQ(conflict->length, 40U);
  ASSERT_TRUE(second_->GetPosix(234, 200, {50, 1, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 5678);
  EXPECT_EQ(conflict->start, 40U);
  EXPECT_EQ(conflict->length, 60U);

  // A later partial unlock must retain both original PIDs on the fragments.
  ASSERT_TRUE(first_->SetPosix(234, 100, {20, 60, PosixType::kUnlock}).ok());
  ASSERT_TRUE(second_->GetPosix(234, 200, {10, 1, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 1234);
  EXPECT_EQ(conflict->length, 20U);
  ASSERT_TRUE(second_->GetPosix(234, 200, {90, 1, PosixType::kWrite}, &conflict).ok());
  ASSERT_TRUE(conflict.has_value());
  EXPECT_EQ(conflict->pid, 5678);
  EXPECT_EQ(conflict->start, 80U);
}

TEST_F(RedisLockAuthorityTest, MalformedStoredPosixPidFailsClosed) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const auto hash = key.PosixLock(235);
  const auto owner = std::to_string(first_lease_->generation()) + ":100";
  std::optional<RedisLockAuthority::PosixConflict> conflict;
  for (const char *value : {"P1|W,0,100", "P2|W,0,100", "P2|W,0,100,2147483648", "P2|W,0,100,-1",
                            "P2|W,0,100,1234|W,100,200,1234", "P2|W,100,100,1"}) {
    SCOPED_TRACE(value);
    redis.hset(hash, owner, value);
    EXPECT_FALSE(second_->GetPosix(235, 200, {0, 200, PosixType::kWrite}, &conflict).ok());
    EXPECT_FALSE(second_->SetPosix(235, 200, {0, 200, PosixType::kWrite}).ok());
  }
  redis.del(hash);
  EXPECT_TRUE(second_->SetPosix(235, 200, {0, 200, PosixType::kWrite}).ok());
}

TEST_F(RedisLockAuthorityTest, FiberAdapterValidatesAndRepliesWithRemoteConflicts) {
  utils::BlockingExecutor executor(2, "lock-adapter-test");
  fuse::FuseAdvisoryLocks first(*first_, executor);
  fuse::FuseAdvisoryLocks second(*second_, executor);

  struct flock request{};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 10;
  request.l_len = 20;
  request.l_pid = 4321;
  test::RunInTestFiber([&] {
    ASSERT_TRUE(first.SetPosix(230, 150, request).ok());
    struct flock result{};
    ASSERT_TRUE(second.GetPosix(230, 150, request, &result).ok());
    EXPECT_EQ(result.l_type, F_WRLCK);
    EXPECT_EQ(result.l_start, 10);
    EXPECT_EQ(result.l_len, 20);
    EXPECT_EQ(result.l_whence, SEEK_SET);
    EXPECT_EQ(result.l_pid, 4321);
    EXPECT_TRUE(first.CanGrant());

    ASSERT_TRUE(first.GetPosix(230, 150, request, &result).ok());
    EXPECT_EQ(result.l_type, F_UNLCK);
    request.l_type = F_RDLCK;
    EXPECT_EQ(second.SetPosix(230, 200, request).ToErrno(), EAGAIN);

    request.l_start = 30;
    ASSERT_TRUE(second.SetPosix(230, 200, request).ok());
    ASSERT_TRUE(second.GetPosix(230, 200, request, &result).ok());
    EXPECT_EQ(result.l_type, F_UNLCK);

    request.l_whence = SEEK_CUR;
    EXPECT_EQ(second.SetPosix(230, 200, request).ToErrno(), EINVAL);
    request.l_whence = SEEK_SET;
    request.l_start = -1;
    EXPECT_EQ(second.SetPosix(230, 200, request).ToErrno(), EINVAL);
    request.l_start = 1;
    request.l_len = -1;
    EXPECT_EQ(second.SetPosix(230, 200, request).ToErrno(), EINVAL);
    request.l_start = std::numeric_limits<off_t>::max();
    request.l_len = 2;
    EXPECT_EQ(second.SetPosix(230, 200, request).ToErrno(), EINVAL);
    request.l_start = 0;
    request.l_len = 0;
    request.l_type = F_UNLCK;
    EXPECT_EQ(second.GetPosix(230, 200, request, &result).ToErrno(), EINVAL);
    request.l_type = 99;
    EXPECT_EQ(second.SetPosix(230, 200, request).ToErrno(), EINVAL);
    request.l_type = F_RDLCK;
    EXPECT_EQ(second.GetPosix(230, 200, request, nullptr).ToErrno(), EINVAL);
  });
}

TEST_F(RedisLockAuthorityTest, FiberAdapterFullOwnerCloseReleasesAllRangesAndChecksLease) {
  utils::BlockingExecutor executor(2, "lock-close-test");
  fuse::FuseAdvisoryLocks first(*first_, executor);
  fuse::FuseAdvisoryLocks second(*second_, executor);
  struct flock request{};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_len = 10;
  request.l_pid = 500;
  test::RunInTestFiber([&] {
    ASSERT_TRUE(first.SetPosix(232, 123, request).ok());
    request.l_start = 20;
    ASSERT_TRUE(first.SetPosix(232, 123, request).ok());
    request.l_start = 0;
    request.l_len = 0;
    EXPECT_EQ(second.SetPosix(232, 456, request).ToErrno(), EAGAIN);
    ASSERT_TRUE(first.UnlockPosixOwner(232, 123).ok());
    EXPECT_TRUE(second.SetPosix(232, 456, request).ok());
    EXPECT_TRUE(first.UnlockPosixOwner(232, 123).ok());  // Mandatory safe no-op close.
    // Preserve a separately tracked owner to verify lease failure remains
    // visible when an actual remote cleanup is still required.
    EXPECT_TRUE(first.SetPosix(233, 123, request).ok());
  });

  ASSERT_TRUE(first_lease_->Revoke().ok());
  test::RunInTestFiber([&] {
    EXPECT_FALSE(first.CanGrant());
    EXPECT_TRUE(first.UnlockPosixOwner(232, 123).ok());          // Already released; no Redis operation.
    EXPECT_EQ(first.UnlockPosixOwner(233, 123).ToErrno(), EIO);  // Still tracked; invalid lease fails.
    struct flock result{};
    EXPECT_EQ(first.GetPosix(232, 123, request, &result).ToErrno(), EIO);
  });
}

TEST_F(RedisLockAuthorityTest, FlockSharedExclusiveAndReleaseAcrossMounts) {
  ASSERT_TRUE(first_->SetFlock(24, 101, FlockType::kShared).ok());
  EXPECT_TRUE(second_->SetFlock(24, 202, FlockType::kShared).ok());
  EXPECT_EQ(second_->SetFlock(24, 202, FlockType::kExclusive).ToErrno(), EAGAIN);
  EXPECT_TRUE(first_->SetFlock(24, 101, FlockType::kUnlock).ok());
  EXPECT_TRUE(second_->SetFlock(24, 202, FlockType::kExclusive).ok());
}

TEST_F(RedisLockAuthorityTest, PosixAndFlockAreDistinctLockNamespaces) {
  // #431/#432 use the semantic lock interface, never the Redis key codec.
  ILockAuthority &first = *first_;
  ILockAuthority &second = *second_;
  ASSERT_TRUE(first.SetPosix(25, 101, {0, 0, PosixType::kWrite}).ok());
  EXPECT_TRUE(second.SetFlock(25, 202, FlockType::kExclusive).ok());
}

TEST_F(RedisLockAuthorityTest, NoOpAndFinalUnlockLeaveNoTombstone) {
  ASSERT_TRUE(first_->SetPosix(26, 101, {0, 0, PosixType::kUnlock}).ok());
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  EXPECT_EQ(redis.exists(key.PosixLock(26)), 0);
  ASSERT_TRUE(first_->SetPosix(26, 101, {0, 0, PosixType::kWrite}).ok());
  ASSERT_TRUE(first_->SetPosix(26, 101, {0, 0, PosixType::kUnlock}).ok());
  EXPECT_EQ(redis.exists(key.PosixLock(26)), 0);
}

TEST_F(RedisLockAuthorityTest, NoOpUnlockInvalidatesAnOlderDelayedGrant) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  // Simulate an ambiguous older WATCH/EXEC: the original grant has been
  // queued but its EXEC arrives only after the newer unlock has returned.
  // A bare HDEL of a nonexistent field does NOT invalidate WATCH in Redis.
  for (const bool flock : {false, true}) {
    const uint64_t ino = flock ? 137 : 136;
    const std::string hash = flock ? key.FlockLock(ino) : key.PosixLock(ino);
    const std::string field = std::to_string(first_lease_->generation()) + ":101";
    {
      // redis++ keeps the transaction's pooled connection until its
      // destructor. Never borrow from `redis` for the final EXISTS while
      // `delayed` is still alive (the test pool has one connection).
      auto delayed = redis.transaction(false, false);
      {
        auto guarded = delayed.redis();
        guarded.watch(hash);
        EXPECT_EQ(guarded.hlen(hash), 0);
      }
      delayed.hset(hash, field, flock ? "F1|X" : "P2|W,0,100,0");

      if (flock) {
        ASSERT_TRUE(first_->SetFlock(ino, 101, FlockType::kUnlock).ok());
      } else {
        ASSERT_TRUE(first_->SetPosix(ino, 101, {0, 100, PosixType::kUnlock}).ok());
      }
      EXPECT_THROW(delayed.exec(), sw::redis::WatchError);
    }
    EXPECT_EQ(redis.exists(hash), 0);
  }
}

TEST_F(RedisLockAuthorityTest, MissingLeaseRejectsGetlkAndMutation) {
  ASSERT_TRUE(second_lease_->Revoke().ok());
  std::optional<RedisLockAuthority::PosixConflict> conflict;
  EXPECT_FALSE(second_->GetPosix(27, 202, {0, 0, PosixType::kRead}, &conflict).ok());
  EXPECT_FALSE(second_->SetPosix(27, 202, {0, 0, PosixType::kRead}).ok());
  EXPECT_FALSE(second_->SetFlock(27, 202, FlockType::kExclusive).ok());
}

TEST_F(RedisLockAuthorityTest, NormalReleaseDoesNotKeepHashOrTombstoneForUsedInodes) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  for (uint64_t ino = 100; ino < 132; ++ino) {
    ASSERT_TRUE(first_->SetPosix(ino, 101, {0, 0, PosixType::kRead}).ok());
    ASSERT_TRUE(first_->SetPosix(ino, 101, {0, 0, PosixType::kUnlock}).ok());
    EXPECT_EQ(redis.exists(key.PosixLock(ino)), 0);
    ASSERT_TRUE(first_->SetFlock(ino, 101, FlockType::kShared).ok());
    ASSERT_TRUE(first_->SetFlock(ino, 101, FlockType::kUnlock).ok());
    EXPECT_EQ(redis.exists(key.FlockLock(ino)), 0);
  }
}

TEST_F(RedisLockAuthorityTest, InvalidRangesCannotProduceAChange) {
  EXPECT_EQ(first_->SetPosix(28, 101, {UINT64_MAX, 2, PosixType::kWrite}).ToErrno(), EINVAL);
  EXPECT_EQ(first_->SetPosix(28, 101, {0, 1, PosixType::kWrite, -1}).ToErrno(), EINVAL);
  EXPECT_EQ(first_->SetPosix(28, 101, {0, 0, PosixType::kUnlock}).ToErrno(), 0);
}

TEST_F(RedisLockAuthorityTest, ExpiredGenerationsDoNotBlockGrantsAndCanBeCleanedInBoundedBatches) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const auto hash = key.PosixLock(30);
  const auto stale_generation = first_lease_->generation();
  // Expired owner records are deliberately left by the O(1)-key Revoke.
  ASSERT_TRUE(first_lease_->Revoke().ok());
  for (uint64_t i = 0; i < 1124; ++i) {
    redis.hset(hash, std::to_string(stale_generation) + ":" + std::to_string(i), "P2|W,0,100,0");
  }
  EXPECT_EQ(second_->SetPosix(30, 5000, {0, 100, PosixType::kWrite}).ToErrno(), ENOLCK);
  EXPECT_LT(redis.hlen(hash), 1124);
  for (int attempt = 0; attempt < 16 && redis.hlen(hash) > 1024; ++attempt) {
    EXPECT_EQ(second_->SetPosix(30, 5000, {0, 100, PosixType::kWrite}).ToErrno(), ENOLCK);
  }
  EXPECT_LE(redis.hlen(hash), 1024);
  EXPECT_TRUE(second_->SetPosix(30, 5000, {0, 100, PosixType::kWrite}).ok());
}

TEST_F(RedisLockAuthorityTest, MixedLiveFirstScanPageStillReclaimsLaterStaleOwners) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const auto hash = key.PosixLock(160);

  // The first page's keys are selected by Redis, not by insertion order.
  // Start with 1,152 distinct expired generations; make ONLY the exact keys
  // on Redis' first HSCAN page live. This reproduces a stable live-only first
  // page with expired owners beyond the page, independent of Hash ordering.
  for (uint64_t i = 0; i < 1152; ++i) {
    redis.hset(hash, std::to_string(800000 + i) + ":1", "P2|R,0,100,0");
  }
  std::vector<std::pair<std::string, std::string>> first_page;
  const auto next = redis.hscan(hash, 0, 128, std::back_inserter(first_page));
  ASSERT_NE(next, 0);
  ASSERT_FALSE(first_page.empty());
  ASSERT_LT(first_page.size(), 1024);
  for (const auto &[field, value] : first_page) {
    const auto generation = field.substr(0, field.find(':'));
    const auto lease_key = key.OwnerLease(std::stoull(generation));
    ASSERT_EQ(redis.command<std::string>("SET", lease_key, "active", "PX", 600000), "OK");
  }

  // An unchanged first scan page must not make the GC repeatedly inspect
  // only live owners forever. Cleanup may be asynchronous to the failed
  // authorization (ENOLCK) but bounded repeated calls must make progress.
  EXPECT_EQ(second_->SetPosix(160, 9999, {0, 100, PosixType::kRead}).ToErrno(), ENOLCK);
  EXPECT_EQ(redis.hlen(hash), 1152);
  bool admitted = false;
  for (int attempt = 0; attempt < 32; ++attempt) {
    const auto status = second_->SetPosix(160, 9999, {0, 100, PosixType::kRead});
    if (status.ok()) {
      admitted = true;
      break;
    }
    EXPECT_EQ(status.ToErrno(), ENOLCK) << attempt;
  }
  EXPECT_TRUE(admitted) << "GC failed to advance beyond the initial live-only HSCAN page";
  EXPECT_LE(redis.hlen(hash), 1024);
  for (const auto &[field, value] : first_page) {
    EXPECT_EQ(redis.hget(hash, field).value_or(""), value);
  }
}

TEST_F(RedisLockAuthorityTest, MalformedOrUnexpectedHashValuesFailClosed) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const auto hash = key.PosixLock(31);
  redis.set(hash, "unexpected type");
  EXPECT_FALSE(first_->SetPosix(31, 1, {0, 0, PosixType::kRead}).ok());
  redis.del(hash);
  redis.hset(hash, std::to_string(first_lease_->generation()) + ":99", "invalid POSIX record");
  EXPECT_FALSE(first_->SetPosix(31, 1, {0, 0, PosixType::kRead}).ok());
  redis.del(hash);
  redis.hset(hash, "!malformed-owner", "P2|W,0,100,0");
  EXPECT_FALSE(first_->GetPosix(31, 1, {0, 100, PosixType::kWrite}, nullptr).ok());
  std::optional<RedisLockAuthority::PosixConflict> conflict;
  EXPECT_FALSE(first_->GetPosix(31, 1, {0, 100, PosixType::kWrite}, &conflict).ok());
}

TEST_F(RedisLockAuthorityTest, MalformedRecordFromExpiredGenerationIsNotAnActiveConflict) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const auto stale_generation = first_lease_->generation();
  ASSERT_TRUE(first_lease_->Revoke().ok());
  redis.hset(key.PosixLock(32), std::to_string(stale_generation) + ":42", "corrupt stale POSIX bytes");
  redis.hset(key.FlockLock(32), std::to_string(stale_generation) + ":42", "corrupt stale flock bytes");
  EXPECT_TRUE(second_->SetPosix(32, 1, {0, 100, PosixType::kWrite}).ok());
  EXPECT_TRUE(second_->SetFlock(32, 1, FlockType::kExclusive).ok());
}

TEST_F(RedisLockAuthorityTest, DistinctGenerationLeasesAreValidatedAsOneWatchedBatch) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const auto hash = key.PosixLock(33);
  std::vector<std::string> synthetic_leases;
  for (uint64_t generation = 10000; generation < 10128; ++generation) {
    const auto lease_key = key.OwnerLease(generation);
    ASSERT_EQ(redis.command<std::string>("SET", lease_key, "active", "PX", 600000), "OK");
    synthetic_leases.push_back(lease_key);
    redis.hset(hash, std::to_string(generation) + ":1", "P2|R,0,100,0");
  }
  EXPECT_TRUE(first_->SetPosix(33, 99, {0, 100, PosixType::kRead}).ok());
  EXPECT_EQ(first_->SetPosix(33, 99, {0, 100, PosixType::kWrite}).ToErrno(), EAGAIN);
  for (const auto &lease_key : synthetic_leases) {
    redis.del(lease_key);
  }
  EXPECT_TRUE(first_->SetPosix(33, 99, {0, 100, PosixType::kWrite}).ok());
}

TEST_F(RedisLockAuthorityTest, PersistedLeaseWithoutTtlIsNeverAssumedExpired) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  redis.set(key.OwnerLease(500000), "corrupted perpetual lease");
  redis.hset(key.PosixLock(34), "500000:1", "P2|W,0,100,0");
  EXPECT_FALSE(first_->SetPosix(34, 99, {0, 100, PosixType::kRead}).ok());
  EXPECT_EQ(redis.hlen(key.PosixLock(34)), 1);
}

TEST_F(RedisLockAuthorityTest, HolderLimitRejectsNewGrantsBeforeCrossingCap) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  const uint64_t synthetic_generation = 500001;
  const auto lease_key = key.OwnerLease(synthetic_generation);
  ASSERT_EQ(redis.command<std::string>("SET", lease_key, "active", "PX", 600000), "OK");

  // The Hash can be full of compatible live holders. An additional owner
  // must fail closed instead of silently publishing the 1,025th field.
  for (const bool flock : {false, true}) {
    const uint64_t ino = flock ? 138 : 139;
    const auto hash = flock ? key.FlockLock(ino) : key.PosixLock(ino);
    for (uint64_t owner = 0; owner < 1024; ++owner) {
      redis.hset(hash, std::to_string(synthetic_generation) + ":" + std::to_string(owner),
                 flock ? "F1|S" : "P2|R,0,100,0");
    }
    ASSERT_EQ(redis.hlen(hash), 1024);
    const auto attempt = flock ? first_->SetFlock(ino, 5000, FlockType::kShared)
                               : first_->SetPosix(ino, 5000, {0, 100, PosixType::kRead});
    EXPECT_EQ(attempt.ToErrno(), ENOLCK);
    EXPECT_EQ(redis.hlen(hash), 1024);

    // At capacity, replacing an existing owner does not consume another
    // slot. A freed slot must allow a compatible new grant.
    redis.hdel(hash, std::to_string(synthetic_generation) + ":0");
    EXPECT_TRUE((flock ? first_->SetFlock(ino, 5000, FlockType::kShared)
                       : first_->SetPosix(ino, 5000, {0, 100, PosixType::kRead}))
                    .ok());
    EXPECT_EQ(redis.hlen(hash), 1024);
  }
}

TEST_F(RedisLockAuthorityTest, LostExecReplyReplaysTheSamePosixIntentSafely) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  for (const auto fault :
       {RedisLockFaultProxy::Fault::kAppliedLostReply, RedisLockFaultProxy::Fault::kNotAppliedLostReply}) {
    RedisLockFaultProxy proxy(config_.host, config_.port, fault, 1);
    auto via_proxy = config_;
    via_proxy.port = proxy.port();
    via_proxy.retry_attempts = 1;
    {
      RedisLockAuthority authority(via_proxy, volume_, *first_lease_);
      const uint64_t inode = fault == RedisLockFaultProxy::Fault::kAppliedLostReply ? 150 : 151;
      ASSERT_TRUE(authority.SetPosix(inode, 600, {0, 100, PosixType::kWrite}).ok());
      const auto hash = key.PosixLock(inode);
      EXPECT_EQ(redis.hlen(hash), 1);
      EXPECT_EQ(redis.hget(hash, std::to_string(first_lease_->generation()) + ":600").value_or(""), "P2|W,0,100,0");
    }
    EXPECT_EQ(proxy.intercepted_execs(), 1);
  }
}

TEST_F(RedisLockAuthorityTest, LostReadOnlyExecDoesNotPublishUnvalidatedGetlk) {
  ASSERT_TRUE(first_->SetPosix(154, 601, {0, 100, PosixType::kWrite}).ok());

  RedisLockFaultProxy proxy(config_.host, config_.port, RedisLockFaultProxy::Fault::kAppliedLostReply, 1);
  auto via_proxy = config_;
  via_proxy.port = proxy.port();
  via_proxy.retry_attempts = 1;
  {
    RedisLockAuthority authority(via_proxy, volume_, *second_lease_);
    std::optional<RedisLockAuthority::PosixConflict> conflict;
    const auto status = authority.GetPosix(154, 602, {0, 100, PosixType::kRead}, &conflict);
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_FALSE(conflict.has_value());
  }
  EXPECT_EQ(proxy.intercepted_execs(), 1);

  std::optional<RedisLockAuthority::PosixConflict> verified;
  ASSERT_TRUE(second_->GetPosix(154, 602, {0, 100, PosixType::kRead}, &verified).ok());
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(verified->kernel_owner, 601);
}

TEST_F(RedisLockAuthorityTest, LostExecReplyRetryExhaustionReturnsOnlyAnError) {
  const redis::RedisKey key(config_.db, volume_);
  sw::redis::Redis redis(ConnectionOptions(config_));
  for (const auto fault :
       {RedisLockFaultProxy::Fault::kAppliedLostReply, RedisLockFaultProxy::Fault::kNotAppliedLostReply}) {
    RedisLockFaultProxy proxy(config_.host, config_.port, fault, 3);
    auto via_proxy = config_;
    via_proxy.port = proxy.port();
    via_proxy.retry_attempts = 1;
    const uint64_t inode = fault == RedisLockFaultProxy::Fault::kAppliedLostReply ? 152 : 153;
    {
      RedisLockAuthority authority(via_proxy, volume_, *first_lease_);
      const auto status = authority.SetFlock(inode, 601, FlockType::kExclusive);
      EXPECT_EQ(status.ToErrno(), EIO);
    }
    EXPECT_EQ(proxy.intercepted_execs(), 3);
    EXPECT_TRUE(first_lease_->CanGrant(first_lease_->generation()))
        << "ambiguous EXEC may not unconditionally revoke the entire owner lease";
    const auto hash = key.FlockLock(inode);
    const auto actual = redis.hget(hash, std::to_string(first_lease_->generation()) + ":601");
    if (fault == RedisLockFaultProxy::Fault::kAppliedLostReply) {
      EXPECT_EQ(actual.value_or(""), "F1|X");
    } else {
      EXPECT_FALSE(actual.has_value());
    }
    // A later explicit request is authoritative; no blind rollback of a
    // possibly committed lock may overwrite this newer state.
    ASSERT_TRUE(first_->SetFlock(inode, 601, FlockType::kShared).ok());
    EXPECT_EQ(redis.hget(hash, std::to_string(first_lease_->generation()) + ":601").value_or(""), "F1|S");
    EXPECT_TRUE(first_->SetFlock(inode, 601, FlockType::kUnlock).ok());
    EXPECT_EQ(redis.exists(hash), 0);
  }
}

TEST_F(RedisLockAuthorityTest, RangeBudgetDoesNotPartiallyPublishOversizedOwner) {
  for (uint64_t offset = 0; offset < 128; ++offset) {
    ASSERT_TRUE(first_->SetPosix(35, 99, {offset * 4, 1, PosixType::kWrite}).ok()) << offset;
  }
  EXPECT_EQ(first_->SetPosix(35, 99, {1024, 1, PosixType::kWrite}).ToErrno(), ENOLCK);
  EXPECT_TRUE(first_->SetPosix(35, 99, {0, 0, PosixType::kUnlock}).ok());
  EXPECT_TRUE(first_->SetPosix(35, 99, {0, 1, PosixType::kWrite}).ok());
}

}  // namespace
}  // namespace swordfs::metadata
