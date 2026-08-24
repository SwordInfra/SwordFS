// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FiberThreadPool.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <folly/io/async/EventBase.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>

#include "utils/FiberThreadPool.hpp"

using swordfs::utils::FiberThreadPool;

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

// ────────────────────────────────────────────────────────────────
// Basic Run
// ────────────────────────────────────────────────────────────────

TEST(FiberThreadPoolTest, RejectsNonFiberCaller) {
  FiberThreadPool pool(1);
  EXPECT_DEATH(pool.Run([] { return 42; }), "must be called from a fiber");
}

TEST(FiberThreadPoolTest, RunReturnsValue) {
  FiberThreadPool pool(2);
  RunInFiber([&] {
    int result = pool.Run([] { return 42; });
    EXPECT_EQ(result, 42);
  });
}

TEST(FiberThreadPoolTest, RunReturnsString) {
  FiberThreadPool pool(2);
  RunInFiber([&] {
    std::string result = pool.Run([] { return std::string("hello"); });
    EXPECT_EQ(result, "hello");
  });
}

TEST(FiberThreadPoolTest, RunVoidFunction) {
  FiberThreadPool pool(2);
  int counter = 0;
  RunInFiber([&] { pool.Run([&counter] { ++counter; }); });
  EXPECT_EQ(counter, 1);
}

// ────────────────────────────────────────────────────────────────
// Concurrency
// ────────────────────────────────────────────────────────────────

TEST(FiberThreadPoolTest, ConcurrentTasks) {
  FiberThreadPool pool(4);
  std::atomic<int> counter{0};
  constexpr int kTasks = 100;

  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  std::atomic<int> completed{0};
  for (int i = 0; i < kTasks; ++i) {
    manager.addTask([&] {
      pool.Run([&counter] {
        ++counter;
        return 0;
      });
      if (++completed == kTasks) {
        done.post();
      }
    });
  }
  while (!done.try_wait()) {
    evb.loopOnce();
  }

  EXPECT_EQ(counter.load(), kTasks);
}

TEST(FiberThreadPoolTest, TasksRunInParallel) {
  constexpr int kThreads = 2;
  FiberThreadPool pool(kThreads);
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};

  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  std::atomic<int> completed{0};
  for (int i = 0; i < kThreads; ++i) {
    manager.addTask([&] {
      pool.Run([&concurrent, &max_concurrent] {
        int c = ++concurrent;
        int expected = max_concurrent.load();
        while (expected < c) {
          max_concurrent.compare_exchange_weak(expected, c);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --concurrent;
        return 0;
      });
      if (++completed == kThreads) {
        done.post();
      }
    });
  }
  while (!done.try_wait()) {
    evb.loopOnce();
  }

  EXPECT_EQ(max_concurrent.load(), kThreads);
}

TEST(FiberThreadPoolTest, BlockingWorkDoesNotBlockOtherFibers) {
  FiberThreadPool pool(1);
  std::atomic<bool> started{false};
  std::atomic<bool> other_fiber_ran{false};

  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  manager.addTask([&] {
    pool.Run([&] {
      started.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    done.post();
  });
  manager.addTask([&] {
    while (!started.load()) {
      folly::fibers::yield();
    }
    other_fiber_ran.store(true);
  });

  while (!done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(other_fiber_ran.load());
}
