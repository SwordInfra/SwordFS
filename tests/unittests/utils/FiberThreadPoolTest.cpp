// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FiberThreadPool.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "utils/FiberThreadPool.hpp"

using swordfs::utils::FiberThreadPool;

// ────────────────────────────────────────────────────────────────
// Basic Run
// ────────────────────────────────────────────────────────────────

TEST(FiberThreadPoolTest, RunReturnsValue) {
  FiberThreadPool pool(2);
  int result = pool.Run([] { return 42; });
  EXPECT_EQ(result, 42);
}

TEST(FiberThreadPoolTest, RunReturnsString) {
  FiberThreadPool pool(2);
  std::string result = pool.Run([] { return std::string("hello"); });
  EXPECT_EQ(result, "hello");
}

TEST(FiberThreadPoolTest, RunVoidFunction) {
  FiberThreadPool pool(2);
  int counter = 0;
  pool.Run([&counter] { ++counter; });
  EXPECT_EQ(counter, 1);
}

// ────────────────────────────────────────────────────────────────
// Concurrency
// ────────────────────────────────────────────────────────────────

TEST(FiberThreadPoolTest, ConcurrentTasks) {
  FiberThreadPool pool(4);
  std::atomic<int> counter{0};
  constexpr int kTasks = 100;

  std::vector<std::thread> threads;
  for (int i = 0; i < kTasks; ++i) {
    threads.emplace_back([&pool, &counter] {
      pool.Run([&counter] {
        ++counter;
        return 0;
      });
    });
  }
  for (auto &t : threads) t.join();

  EXPECT_EQ(counter.load(), kTasks);
}

TEST(FiberThreadPoolTest, TasksRunInParallel) {
  constexpr int kThreads = 2;
  FiberThreadPool pool(kThreads);
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&pool, &concurrent, &max_concurrent] {
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
    });
  }
  for (auto &t : threads) t.join();

  EXPECT_GE(max_concurrent.load(), 1);
}
