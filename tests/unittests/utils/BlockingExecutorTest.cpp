// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

using swordfs::utils::BlockingExecutor;
using swordfs::utils::ExecutionDomain;

template <typename Fn>
void RunInFiber(Fn &&fn) {
  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  manager.addTask([&] {
    fn();
    done.post();
  });
  while (!done.try_wait()) {
    evb.loopOnce();
  }
}

TEST(BlockingExecutorTest, RejectsZeroWorkers) {
  EXPECT_DEATH({ BlockingExecutor executor(0); }, "requires at least one worker");
}

TEST(BlockingExecutorTest, RunFromThreadReturnsValue) {
  BlockingExecutor executor(1);
  EXPECT_EQ(executor.RunFromThread([] { return 42; }), 42);
}

TEST(BlockingExecutorTest, ShutdownIsIdempotentAndRejectsNewWork) {
  BlockingExecutor executor(1);
  EXPECT_EQ(executor.RunFromThread([] { return 42; }), 42);
  executor.Shutdown();
  executor.Shutdown();
  EXPECT_DEATH({ executor.RunFromThread([] {}); }, "BlockingExecutor is shut down");
}

TEST(BlockingExecutorTest, RunFromFiberReturnsValue) {
  BlockingExecutor executor(2);
  RunInFiber([&] { EXPECT_EQ(executor.RunFromFiber([] { return 42; }), 42); });
}

TEST(BlockingExecutorTest, WorkerRunsInThreadDomainFromBothCallDomains) {
  BlockingExecutor executor(1);
  EXPECT_EQ(executor.RunFromThread([] { return swordfs::utils::CurrentExecutionDomain(); }), ExecutionDomain::kThread);
  RunInFiber([&] {
    EXPECT_EQ(executor.RunFromFiber([] { return swordfs::utils::CurrentExecutionDomain(); }), ExecutionDomain::kThread);
  });
}

TEST(BlockingExecutorTest, RunFromFiberReturnsStringAndVoid) {
  BlockingExecutor executor(2);
  RunInFiber([&] {
    EXPECT_EQ(executor.RunFromFiber([] { return std::string("hello"); }), "hello");
    int counter = 0;
    executor.RunFromFiber([&] { ++counter; });
    EXPECT_EQ(counter, 1);
  });
}

TEST(BlockingExecutorTest, PropagatesException) {
  BlockingExecutor executor(1);
  EXPECT_THROW(executor.RunFromThread([] { throw std::runtime_error("thread failure"); }), std::runtime_error);
  RunInFiber([&] {
    EXPECT_THROW(executor.RunFromFiber([] { throw std::runtime_error("fiber failure"); }), std::runtime_error);
  });
}

TEST(BlockingExecutorTest, FiberCallersRunBlockingTasksInParallel) {
  constexpr int kThreads = 2;
  constexpr auto kCoordinationWatchdog = std::chrono::seconds(5);
  BlockingExecutor executor(kThreads);
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};
  std::atomic<int> workers_entered{0};
  std::atomic<bool> watchdog_expired{false};
  std::promise<void> all_workers_entered;
  auto all_workers_future = all_workers_entered.get_future().share();

  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  std::atomic<int> completed{0};
  for (int i = 0; i < kThreads; ++i) {
    manager.addTask([&] {
      executor.RunFromFiber([&] {
        const int active = ++concurrent;
        int expected = max_concurrent.load();
        while (expected < active && !max_concurrent.compare_exchange_weak(expected, active)) {
        }
        if (workers_entered.fetch_add(1, std::memory_order_acq_rel) + 1 == kThreads) {
          all_workers_entered.set_value();
        }
        // Coordinate on an event rather than a sleep. The deadline is only a
        // liveness watchdog so a broken single-worker implementation fails
        // instead of hanging the test indefinitely.
        if (all_workers_future.wait_for(kCoordinationWatchdog) != std::future_status::ready) {
          watchdog_expired.store(true, std::memory_order_release);
        }
        --concurrent;
      });
      if (++completed == kThreads) {
        done.post();
      }
    });
  }
  while (!done.try_wait()) {
    evb.loopOnce();
  }

  EXPECT_FALSE(watchdog_expired.load(std::memory_order_acquire));
  EXPECT_EQ(max_concurrent.load(), kThreads);
}

TEST(BlockingExecutorTest, BlockingWorkDoesNotBlockOtherFibers) {
  constexpr auto kCoordinationWatchdog = std::chrono::seconds(5);
  BlockingExecutor executor(1);
  std::atomic<bool> started{false};
  std::atomic<bool> other_fiber_ran{false};
  std::atomic<bool> watchdog_expired{false};
  std::promise<void> release_blocking;
  auto release_future = release_blocking.get_future();

  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  manager.addTask([&] {
    executor.RunFromFiber([&] {
      started.store(true, std::memory_order_release);
      if (release_future.wait_for(kCoordinationWatchdog) != std::future_status::ready) {
        watchdog_expired.store(true, std::memory_order_release);
      }
    });
    done.post();
  });
  manager.addTask([&] {
    while (!started.load(std::memory_order_acquire)) {
      folly::fibers::yield();
    }
    other_fiber_ran.store(true, std::memory_order_release);
    release_blocking.set_value();
  });

  while (!done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_FALSE(watchdog_expired.load(std::memory_order_acquire));
  EXPECT_TRUE(other_fiber_ran.load(std::memory_order_acquire));
}

#ifndef NDEBUG
TEST(BlockingExecutorTest, RunFromFiberRejectsThreadCaller) {
  BlockingExecutor executor(1);
  EXPECT_DEATH(
      { executor.RunFromFiber([] {}); }, "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}

TEST(BlockingExecutorTest, RunFromThreadRejectsFiberCaller) {
  BlockingExecutor executor(1);
  EXPECT_DEATH(
      { RunInFiber([&] { executor.RunFromThread([] {}); }); },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}
#endif
