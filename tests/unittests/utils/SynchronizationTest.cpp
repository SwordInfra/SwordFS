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

TEST(SynchronizationTest, ExecutionDomainDistinguishesFiberAndThread) {
  EXPECT_EQ(CurrentExecutionDomain(), ExecutionDomain::kThread);

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  fm.addTask([&] {
    EXPECT_EQ(CurrentExecutionDomain(), ExecutionDomain::kFiber);
    done.post();
  });
  while (!done.try_wait()) {
    evb.loopOnce();
  }
}

TEST(SynchronizationTest, FiberMutexAllowsFiberDomain) {
  FiberMutex mutex;
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

TEST(SynchronizationTest, ThreadMutexAllowsThreadDomain) {
  ThreadMutex mutex;
  std::lock_guard<ThreadMutex> lock(mutex);
}

#ifndef NDEBUG
TEST(SynchronizationTest, FiberMutexRejectsThreadDomain) {
  EXPECT_DEATH(
      {
        FiberMutex mutex;
        mutex.lock();
      },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}

TEST(SynchronizationTest, FiberMutexUnlockRejectsThreadDomain) {
  EXPECT_DEATH(
      {
        FiberMutex mutex;
        folly::EventBase evb;
        auto &fm = folly::fibers::getFiberManager(evb);
        folly::fibers::Baton locked;
        fm.addTask([&] {
          mutex.lock();
          locked.post();
        });
        while (!locked.try_wait()) {
          evb.loopOnce();
        }
        mutex.unlock();
      },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}

TEST(SynchronizationTest, FiberRWMutexUnlockSharedRejectsThreadDomain) {
  EXPECT_DEATH(
      {
        FiberRWMutex mutex;
        folly::EventBase evb;
        auto &fm = folly::fibers::getFiberManager(evb);
        folly::fibers::Baton locked;
        fm.addTask([&] {
          mutex.lock_shared();
          locked.post();
        });
        while (!locked.try_wait()) {
          evb.loopOnce();
        }
        mutex.unlock_shared();
      },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
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
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}

TEST(SynchronizationTest, ThreadMutexUnlockRejectsFiberDomain) {
  EXPECT_DEATH(
      {
        ThreadMutex mutex;
        mutex.lock();
        folly::EventBase evb;
        auto &fm = folly::fibers::getFiberManager(evb);
        fm.addTask([&] { mutex.unlock(); });
        evb.loop();
      },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}
#endif

}  // namespace
}  // namespace swordfs::utils
