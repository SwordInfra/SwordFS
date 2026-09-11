// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include "utils/ExecutionDomain.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::utils {
namespace {

TEST(SynchronizationTest, FiberMutexAllowsFiberAndUnknownDomains) {
  FiberMutex mutex;
  {
    std::lock_guard<FiberMutex> lock(mutex);
  }

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  fm.addTask([&] {
    std::lock_guard<FiberMutex> lock(mutex);
    done.post();
  });
  while (!done.try_wait()) {
    evb.loopOnce();
  }
}

TEST(SynchronizationTest, ThreadMutexAllowsBlockingThreadAndUnknownDomains) {
  ThreadMutex mutex;
  {
    std::lock_guard<ThreadMutex> lock(mutex);
  }
  {
    ScopedBlockingThreadDomain domain;
    std::lock_guard<ThreadMutex> lock(mutex);
  }
}

#ifndef NDEBUG
TEST(SynchronizationTest, FiberMutexRejectsBlockingThreadDomain) {
  EXPECT_DEATH(
      {
        ScopedBlockingThreadDomain domain;
        FiberMutex mutex;
        mutex.lock();
      },
      "fiber synchronization primitive used from blocking-thread domain");
}

TEST(SynchronizationTest, ThreadMutexRejectsFiberDomain) {
  EXPECT_DEATH(
      {
        folly::EventBase evb;
        auto &fm = folly::fibers::getFiberManager(evb);
        fm.addTask([] {
          ThreadMutex mutex;
          mutex.lock();
        });
        evb.loop();
      },
      "thread synchronization primitive used from fiber domain");
}
#endif

}  // namespace
}  // namespace swordfs::utils
