// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/CancellationToken.h>
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <sys/file.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "fuse/FuseAdvisoryLocks.hpp"
#include "fuse/LockRetryScheduler.hpp"
#include "metadata/LockAuthority.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/FiberRuntime.hpp"

namespace swordfs::fuse {
namespace {

using namespace std::chrono_literals;

// An external lock authority supplies observable handoffs without introducing
// test-only hooks or state accessors in the production retry scheduler.
class GatedProbeAuthority final : public metadata::ILockAuthority {
 public:
  utils::Status SetPosix(uint64_t, uint64_t, PosixRange) override {
    OnAttempt();
    attempts_.fetch_add(1, std::memory_order_relaxed);
    return utils::Status::WouldBlock("held by another mount");
  }
  utils::Status GetPosix(uint64_t, uint64_t, PosixRange, std::optional<PosixConflict> *) override {
    return utils::Status::OperationNotSupported("unused by wait tests");
  }
  utils::Status SetFlock(uint64_t, uint64_t, FlockType) override {
    OnAttempt();
    attempts_.fetch_add(1, std::memory_order_relaxed);
    return utils::Status::WouldBlock("held by another mount");
  }
  utils::Status Inspect(uint64_t inode, bool flock, std::vector<LockHolder> *holders) override {
    std::unique_lock lock(mutex_);
    const size_t number = ++probes_;
    visited_.emplace_back(inode, flock);
    cv_.notify_all();
    // A probe already in flight cannot finish until the test explicitly
    // opens it, so a cancellation race is synchronized with real work.
    cv_.wait(lock, [&] { return number <= released_; });
    if (failure_.has_value()) {
      return *failure_;
    }
    const auto it = snapshots_.find({inode, flock});
    *holders = it == snapshots_.end() ? std::vector<LockHolder>{} : it->second;
    return utils::Status::OK();
  }
  bool CanGrant() const override {
    return true;
  }

  bool WaitForProbes(size_t count) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 5s, [&] { return probes_ >= count; });
  }
  void ReleaseThrough(size_t count) {
    std::lock_guard lock(mutex_);
    released_ = count;
    cv_.notify_all();
  }
  void ReleaseAll() {
    ReleaseThrough(std::numeric_limits<size_t>::max());
  }
  void OnFirstAttempt(std::function<void()> callback) {
    first_attempt_ = std::move(callback);
  }
  void SetHolders(uint64_t inode, bool flock, std::vector<LockHolder> holders) {
    std::lock_guard lock(mutex_);
    snapshots_[{inode, flock}] = std::move(holders);
  }
  void SetProbeFailure(utils::Status failure) {
    std::lock_guard lock(mutex_);
    failure_ = std::move(failure);
  }
  size_t ProbeCount() {
    std::lock_guard lock(mutex_);
    return probes_;
  }
  size_t UniqueProbedGroups() {
    std::lock_guard lock(mutex_);
    return std::set<std::pair<uint64_t, bool>>(visited_.begin(), visited_.end()).size();
  }
  int Attempts() const {
    return attempts_.load(std::memory_order_relaxed);
  }

 private:
  void OnAttempt() {
    if (first_attempt_) {
      auto callback = std::move(first_attempt_);
      callback();
    }
  }
  std::function<void()> first_attempt_;
  std::mutex mutex_;
  std::condition_variable cv_;
  size_t probes_ = 0;
  size_t released_ = 0;
  std::vector<std::pair<uint64_t, bool>> visited_;
  std::map<std::pair<uint64_t, bool>, std::vector<LockHolder>> snapshots_;
  std::optional<utils::Status> failure_;
  std::atomic<int> attempts_{0};
};

TEST(LockRetrySchedulerTest, NewWaiterStartsActiveWithoutSpuriousNotification) {
  // Default waiter state is the public admission contract, including an
  // unwoken Baton. Cancellation/probe callbacks may begin immediately after
  // registration on another EventBase.
  LockRetryScheduler::Waiter waiter{};
  EXPECT_TRUE(waiter.active.load(std::memory_order_acquire));
  EXPECT_FALSE(waiter.notified.load(std::memory_order_acquire));
  EXPECT_EQ(waiter.probe_error.load(std::memory_order_acquire), 0);
  EXPECT_EQ(waiter.flock_type, metadata::ILockAuthority::FlockType::kUnlock);
  EXPECT_FALSE(waiter.baton.try_wait_for(0ms));
}

TEST(LockRetrySchedulerTest, HolderSnapshotCopiesIndependentRanges) {
  using Authority = metadata::ILockAuthority;
  Authority::LockHolder original{17, false, {{5, 10, Authority::PosixType::kRead, 34}}, Authority::FlockType::kShared};
  const Authority::LockHolder snapshot = original;
  original.ranges[0].start = 99;
  original.flock = Authority::FlockType::kExclusive;
  ASSERT_EQ(snapshot.ranges.size(), 1U);
  EXPECT_EQ(snapshot.kernel_owner, 17U);
  EXPECT_FALSE(snapshot.local_generation);
  EXPECT_EQ(snapshot.ranges[0].start, 5U);
  EXPECT_EQ(snapshot.ranges[0].length, 10U);
  EXPECT_EQ(snapshot.ranges[0].pid, 34);
  EXPECT_EQ(snapshot.flock, Authority::FlockType::kShared);
}

TEST(LockRetrySchedulerTest, CancelDuringInFlightReadOnlyProbeNeverReattemptsGrant) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "gated-lock-probe");
  FuseAdvisoryLocks locks(authority, executor);
  utils::FiberRuntime runtime;
  folly::CancellationSource cancellation;
  std::promise<utils::Status> reply;
  auto result = reply.get_future();
  auto cleanup = folly::makeGuard([&] {
    cancellation.requestCancellation();
    authority.ReleaseAll();
    runtime.Shutdown();
  });

  ASSERT_TRUE(runtime.Submit([&] { reply.set_value(locks.Flock(100, 7, LOCK_EX, cancellation.getToken())); }));
  ASSERT_TRUE(authority.WaitForProbes(1)) << "the request never reached a registered waiter probe";
  EXPECT_EQ(authority.Attempts(), 1);
  cancellation.requestCancellation();
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(result.get().ToErrno(), EINTR);
  authority.ReleaseAll();
  EXPECT_EQ(authority.Attempts(), 1) << "a cancelled request must not retry its Redis mutation";
}

// A holder snapshot is only a hint: another mount can win immediately after
// Inspect returns. The originating Fiber must reattempt the authoritative
// Redis mutation, receive EAGAIN and register a fresh waiter, not treat the
// read-only probe as an ownership grant.
TEST(LockRetrySchedulerTest, ProbeToGrantRaceRetriesWithoutSpeculativeOwnership) {
  for (const bool flock : {false, true}) {
    GatedProbeAuthority authority;
    utils::BlockingExecutor executor(1, "raced-lock-grant");
    FuseAdvisoryLocks locks(authority, executor);
    utils::FiberRuntime runtime;
    folly::CancellationSource cancellation;
    std::promise<utils::Status> reply;
    auto result = reply.get_future();
    auto cleanup = folly::makeGuard([&] {
      cancellation.requestCancellation();
      authority.ReleaseAll();
      runtime.Shutdown();
    });
    struct flock request{};
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    request.l_start = 0;
    request.l_len = 0;
    ASSERT_TRUE(runtime.Submit([&] {
      if (flock) {
        reply.set_value(locks.Flock(101, 7, LOCK_EX, cancellation.getToken()));
      } else {
        reply.set_value(locks.SetPosix(101, 7, request, true, cancellation.getToken()));
      }
    }));
    ASSERT_TRUE(authority.WaitForProbes(1));
    EXPECT_EQ(authority.Attempts(), 1);
    authority.ReleaseThrough(1);
    ASSERT_TRUE(authority.WaitForProbes(2));
    EXPECT_EQ(authority.Attempts(), 2);
    EXPECT_EQ(authority.UniqueProbedGroups(), 1U);
    cancellation.requestCancellation();
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(result.get().ToErrno(), EINTR);
    EXPECT_EQ(authority.Attempts(), 2) << "a canceled retried attempt must not mutate ownership again";
  }
}

TEST(LockRetrySchedulerTest, GroupedSharedWaitersWakeAtMostEightPerProbe) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "grouped-lock-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (uint64_t owner = 1; owner <= 12; ++owner) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 200;
    waiter->kernel_owner = owner;
    waiter->flock = true;
    waiter->flock_type = metadata::ILockAuthority::FlockType::kShared;
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto registration = registered.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    bool ok = true;
    for (const auto &waiter : waiters) {
      ok &= scheduler.RegisterWaiter(waiter).ok();
    }
    registered.set_value(ok);
  }));
  ASSERT_EQ(registration.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(registration.get());
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseThrough(1);
  // The second inspection begins only after the first tick has decided all
  // wakeups. It provides a semantic barrier without a guessed sleep interval.
  ASSERT_TRUE(authority.WaitForProbes(2));
  const auto notified = std::count_if(waiters.begin(), waiters.end(), [](const auto &waiter) {
    return waiter->notified.load(std::memory_order_acquire);
  });
  EXPECT_EQ(notified, 8);
  EXPECT_EQ(authority.UniqueProbedGroups(), 1U);
  authority.ReleaseAll();
}

TEST(LockRetrySchedulerTest, DistinctInodeAndLockClassGroupsMakeProgressUnderFourGroupBudget) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "multi-group-lock-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (uint64_t index = 0; index < 6; ++index) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 300 + index / 2;
    waiter->kernel_owner = index + 1;
    waiter->flock = index % 2 == 0;
    waiter->flock_type = metadata::ILockAuthority::FlockType::kExclusive;
    waiter->range = {0, 0, metadata::ILockAuthority::PosixType::kWrite};
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto registration = registered.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    bool ok = true;
    for (const auto &waiter : waiters) {
      ok &= scheduler.RegisterWaiter(waiter).ok();
    }
    registered.set_value(ok);
  }));
  ASSERT_EQ(registration.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(registration.get());
  authority.ReleaseAll();
  ASSERT_TRUE(authority.WaitForProbes(6));
  EXPECT_EQ(authority.UniqueProbedGroups(), 6U);
}

// The invariant is observable through public waiter admissions; no private
// capacity counter or scheduler test-mode knob is exposed by production code.
TEST(LockRetrySchedulerTest, EnforcesWaiterCapacityWithoutLeakingRejectedAdmissions) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "capacity-lock-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (uint64_t i = 0; i < 1025; ++i) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 500;
    waiter->kernel_owner = i + 1;
    waiter->flock = true;
    waiter->flock_type = metadata::ILockAuthority::FlockType::kExclusive;
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<std::vector<int>> results;
  auto result = results.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    std::vector<int> errors;
    for (const auto &waiter : waiters) {
      errors.push_back(scheduler.RegisterWaiter(waiter).ToErrno());
    }
    results.set_value(std::move(errors));
  }));
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  const auto errors = result.get();
  ASSERT_EQ(errors.size(), 1025U);
  EXPECT_TRUE(std::all_of(errors.begin(), errors.end() - 1, [](int error) { return error == 0; }));
  EXPECT_EQ(errors.back(), ENOLCK);
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseAll();
}

TEST(LockRetrySchedulerTest, SameGenerationOwnerCanRetryButRemoteOwnerCannot) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "owner-filter-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  // Same kernel owner is only exempt if it belongs to the current generation.
  authority.SetHolders(501, true, {{11, true, {}, metadata::ILockAuthority::FlockType::kExclusive}});
  auto local = std::make_shared<LockRetryScheduler::Waiter>();
  local->inode_ino = 501;
  local->kernel_owner = 11;
  local->flock = true;
  local->flock_type = metadata::ILockAuthority::FlockType::kExclusive;
  auto remote = std::make_shared<LockRetryScheduler::Waiter>();
  remote->inode_ino = 501;
  remote->kernel_owner = 22;
  remote->flock = true;
  remote->flock_type = metadata::ILockAuthority::FlockType::kShared;
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      scheduler.RemoveWaiter(local);
      scheduler.RemoveWaiter(remote);
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto result = registered.get_future();
  ASSERT_TRUE(runtime.Submit(
      [&] { registered.set_value(scheduler.RegisterWaiter(local).ok() && scheduler.RegisterWaiter(remote).ok()); }));
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(result.get());
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseThrough(1);
  ASSERT_TRUE(authority.WaitForProbes(2));
  EXPECT_TRUE(local->notified.load(std::memory_order_acquire));
  EXPECT_FALSE(remote->notified.load(std::memory_order_acquire));
}

TEST(LockRetrySchedulerTest, PosixCandidatesRespectHolderRangesAndEachOthersReservations) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "range-filter-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  using Type = metadata::ILockAuthority::PosixType;
  authority.SetHolders(502, false,
                       {{44, false, {{10, 10, Type::kWrite}}, metadata::ILockAuthority::FlockType::kUnlock}});
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (auto [owner, start, length, type] : {
           std::tuple<uint64_t, uint64_t, uint64_t, Type>{1, 0, 5, Type::kWrite},
           {2, 12, 5, Type::kRead},
           {3, 2, 8, Type::kWrite},
           {4, 20, 0, Type::kRead},
           {5, 22, 5, Type::kWrite},
       }) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 502;
    waiter->kernel_owner = owner;
    waiter->range = {start, length, type};
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto result = registered.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    bool success = true;
    for (const auto &waiter : waiters) {
      success &= scheduler.RegisterWaiter(waiter).ok();
    }
    registered.set_value(success);
  }));
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(result.get());
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseThrough(1);
  ASSERT_TRUE(authority.WaitForProbes(2));
  // A record inside the foreign write range is blocked; independent write
  // ranges and a compatible read are allowed into the next grant attempt.
  EXPECT_TRUE(waiters[0]->notified.load());
  EXPECT_FALSE(waiters[1]->notified.load());
  EXPECT_FALSE(waiters[2]->notified.load());
  EXPECT_TRUE(waiters[3]->notified.load());
  EXPECT_FALSE(waiters[4]->notified.load());
}

TEST(LockRetrySchedulerTest, InspectionErrorReachesEachWaiterWithoutAnyGrantMutation) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "failed-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  authority.SetProbeFailure(utils::Status::NoLocks("inspection holder cap exceeded"));
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (uint64_t owner = 1; owner <= 3; ++owner) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 503;
    waiter->kernel_owner = owner;
    waiter->flock = true;
    waiter->flock_type = metadata::ILockAuthority::FlockType::kShared;
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto result = registered.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    bool success = true;
    for (const auto &waiter : waiters) {
      success &= scheduler.RegisterWaiter(waiter).ok();
    }
    registered.set_value(success);
  }));
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(result.get());
  // A separate observing Fiber completes only when the actual waiters have
  // been notified; there is no following probe after a terminal failure.
  std::promise<void> all_notified;
  auto notifications = all_notified.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    for (const auto &waiter : waiters) {
      waiter->baton.wait();
    }
    all_notified.set_value();
  }));
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseThrough(1);
  ASSERT_EQ(notifications.wait_for(5s), std::future_status::ready);
  for (const auto &waiter : waiters) {
    EXPECT_TRUE(waiter->notified.load());
    EXPECT_EQ(waiter->probe_error.load(), ENOLCK);
  }
  EXPECT_EQ(authority.Attempts(), 0);
}

TEST(LockRetrySchedulerTest, LockAdapterPropagatesProbeHolderCapacityError) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "adapter-probe-error");
  FuseAdvisoryLocks locks(authority, executor);
  utils::FiberRuntime runtime;
  authority.SetProbeFailure(utils::Status::NoLocks("holder cap exceeded"));
  std::promise<utils::Status> reply;
  auto result = reply.get_future();
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Shutdown();
  });
  ASSERT_TRUE(runtime.Submit([&] { reply.set_value(locks.Flock(505, 7, LOCK_SH)); }));
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseAll();
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(result.get().ToErrno(), ENOLCK);
  EXPECT_EQ(authority.Attempts(), 1);
}

TEST(LockRetrySchedulerTest, CancellationBetweenFirstAttemptAndWaiterAdmission) {
  for (const bool flock : {false, true}) {
    GatedProbeAuthority authority;
    utils::BlockingExecutor executor(1, "cancel-during-admission");
    FuseAdvisoryLocks locks(authority, executor);
    utils::FiberRuntime runtime;
    folly::CancellationSource cancellation;
    authority.OnFirstAttempt([&] { cancellation.requestCancellation(); });
    std::promise<utils::Status> reply;
    auto result = reply.get_future();
    auto cleanup = folly::makeGuard([&] {
      cancellation.requestCancellation();
      authority.ReleaseAll();
      runtime.Shutdown();
    });
    struct flock request{};
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    ASSERT_TRUE(runtime.Submit([&] {
      reply.set_value(flock ? locks.Flock(506, 7, LOCK_EX, cancellation.getToken())
                            : locks.SetPosix(506, 7, request, true, cancellation.getToken()));
    }));
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(result.get().ToErrno(), EINTR);
    EXPECT_EQ(authority.Attempts(), 1);
    EXPECT_EQ(authority.ProbeCount(), 0U);
  }
}

TEST(LockRetrySchedulerTest, FailedDriverAdmissionPropagatesWithoutLeavingAWaiter) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "driver-admission");
  FuseAdvisoryLocks locks(authority, executor);
  swordfs::test::RunInTestFiber([&] {
    EXPECT_EQ(locks.Flock(507, 1, LOCK_SH).ToErrno(), EIO);
    struct flock request{};
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    EXPECT_EQ(locks.SetPosix(507, 1, request, true).ToErrno(), EIO);
  });
  EXPECT_EQ(authority.Attempts(), 2);
  EXPECT_EQ(authority.ProbeCount(), 0U);
}

// Every ordinary FUSE file close delivers FLUSH, even without a record lock.
// No Redis unlock transaction may be issued for owners that have never made
// a record-lock attempt; generic/531 stresses this path heavily.
TEST(LockRetrySchedulerTest, UnlockedPosixOwnersDoNotIssueRedisFlushTransactions) {
  class CountingAuthority final : public metadata::ILockAuthority {
   public:
    utils::Status SetPosix(uint64_t, uint64_t, PosixRange range) override {
      if (range.type == PosixType::kUnlock) {
        ++unlocks;
      } else if (fail_grants) {
        return utils::Status::IOError("unconfirmed grant outcome");
      }
      return utils::Status::OK();
    }
    utils::Status GetPosix(uint64_t, uint64_t, PosixRange, std::optional<PosixConflict> *) override {
      return utils::Status::OK();
    }
    utils::Status SetFlock(uint64_t, uint64_t, FlockType) override {
      return utils::Status::OK();
    }
    bool CanGrant() const override {
      return true;
    }
    std::atomic<int> unlocks{0};
    bool fail_grants = false;
  } authority;
  utils::BlockingExecutor executor(1, "posix-flush-filter");
  FuseAdvisoryLocks locks(authority, executor);
  swordfs::test::RunInTestFiber([&] {
    for (uint64_t inode = 1; inode <= 100; ++inode) {
      EXPECT_TRUE(locks.UnlockPosixOwner(inode, 7).ok());
    }
    EXPECT_EQ(authority.unlocks.load(), 0) << "ordinary closes must not cause Redis lock RTTs";
    struct flock request{};
    request.l_type = F_WRLCK;
    request.l_whence = SEEK_SET;
    request.l_start = 0;
    request.l_len = 10;
    EXPECT_TRUE(locks.SetPosix(101, 7, request).ok());
    EXPECT_TRUE(locks.UnlockPosixOwner(101, 8).ok());
    EXPECT_EQ(authority.unlocks.load(), 0) << "a different process owner cannot trigger cleanup";
    EXPECT_TRUE(locks.UnlockPosixOwner(101, 7).ok());
    EXPECT_EQ(authority.unlocks.load(), 1);
    EXPECT_TRUE(locks.UnlockPosixOwner(101, 7).ok());
    EXPECT_EQ(authority.unlocks.load(), 1) << "last-close cleanup must not issue duplicate Redis work";

    // A failed grant response may still represent an ambiguous backend
    // commit. Its owner must remain eligible for full-range close cleanup.
    authority.fail_grants = true;
    EXPECT_EQ(locks.SetPosix(101, 9, request).ToErrno(), EIO);
    EXPECT_TRUE(locks.UnlockPosixOwner(101, 9).ok());
    EXPECT_EQ(authority.unlocks.load(), 2);
  });
}

TEST(LockRetrySchedulerTest, SharedRequestsRemainCompatibleWithRemoteSharedFlock) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "shared-holder-probe");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  authority.SetHolders(508, true, {{90, false, {}, metadata::ILockAuthority::FlockType::kShared}});
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (uint64_t owner : {11, 22}) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 508;
    waiter->kernel_owner = owner;
    waiter->flock = true;
    waiter->flock_type = metadata::ILockAuthority::FlockType::kShared;
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto registration = registered.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    bool ok = true;
    for (const auto &waiter : waiters) {
      ok &= scheduler.RegisterWaiter(waiter).ok();
    }
    registered.set_value(ok);
  }));
  ASSERT_EQ(registration.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(registration.get());
  std::promise<void> notified;
  auto notifications = notified.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    for (const auto &waiter : waiters) {
      waiter->baton.wait();
    }
    notified.set_value();
  }));
  ASSERT_TRUE(authority.WaitForProbes(1));
  authority.ReleaseThrough(1);
  ASSERT_EQ(notifications.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(waiters[0]->notified.load());
  EXPECT_TRUE(waiters[1]->notified.load());
}

TEST(LockRetrySchedulerTest, RotatesFourProbesAcrossSixPersistentlyContendedGroups) {
  GatedProbeAuthority authority;
  utils::BlockingExecutor executor(1, "rotating-group-probes");
  LockRetryScheduler scheduler(authority, executor);
  utils::FiberRuntime runtime;
  std::vector<std::shared_ptr<LockRetryScheduler::Waiter>> waiters;
  for (uint64_t i = 0; i < 6; ++i) {
    auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
    waiter->inode_ino = 600 + i;
    waiter->kernel_owner = i + 1;
    waiter->flock = true;
    waiter->flock_type = metadata::ILockAuthority::FlockType::kExclusive;
    authority.SetHolders(waiter->inode_ino, true, {{100, false, {}, metadata::ILockAuthority::FlockType::kExclusive}});
    waiters.push_back(std::move(waiter));
  }
  auto cleanup = folly::makeGuard([&] {
    authority.ReleaseAll();
    runtime.Submit([&] {
      for (const auto &waiter : waiters) {
        scheduler.RemoveWaiter(waiter);
      }
    });
    runtime.Shutdown();
  });
  std::promise<bool> registered;
  auto registration = registered.get_future();
  ASSERT_TRUE(runtime.Submit([&] {
    bool ok = true;
    for (const auto &waiter : waiters) {
      ok &= scheduler.RegisterWaiter(waiter).ok();
    }
    registered.set_value(ok);
  }));
  ASSERT_EQ(registration.wait_for(5s), std::future_status::ready);
  ASSERT_TRUE(registration.get());
  authority.ReleaseAll();
  ASSERT_TRUE(authority.WaitForProbes(8));
  EXPECT_EQ(authority.UniqueProbedGroups(), 6U);
  for (const auto &waiter : waiters) {
    EXPECT_FALSE(waiter->notified.load());
  }
}

}  // namespace
}  // namespace swordfs::fuse
