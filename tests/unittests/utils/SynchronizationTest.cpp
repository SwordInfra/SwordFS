// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "FiberTest.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::utils {
namespace {

TEST(SynchronizationTest, ExecutionDomainDistinguishesFiberAndThread) {
  EXPECT_EQ(CurrentExecutionDomain(), ExecutionDomain::kThread);
  swordfs::test::RunInTestFiber([&] { EXPECT_EQ(CurrentExecutionDomain(), ExecutionDomain::kFiber); });
}

TEST(SynchronizationTest, FiberMutexAllowsFiberDomain) {
  FiberMutex mutex;
  swordfs::test::RunInTestFiber([&] { std::lock_guard<FiberMutex> lock(mutex); });
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
        swordfs::test::RunInTestFiber([&] { mutex.lock(); });
        mutex.unlock();
      },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}

TEST(SynchronizationTest, FiberRWMutexUnlockSharedRejectsThreadDomain) {
  EXPECT_DEATH(
      {
        FiberRWMutex mutex;
        swordfs::test::RunInTestFiber([&] { mutex.lock_shared(); });
        mutex.unlock_shared();
      },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}

TEST(SynchronizationTest, ThreadMutexRejectsFiberDomain) {
  EXPECT_DEATH(
      {
        swordfs::test::RunInTestFiber([] {
          ThreadMutex mutex;
          mutex.lock();
        });
      },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}

TEST(SynchronizationTest, ThreadMutexUnlockRejectsFiberDomain) {
  EXPECT_DEATH(
      {
        ThreadMutex mutex;
        mutex.lock();
        swordfs::test::RunInTestFiber([&] { mutex.unlock(); });
      },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}
#endif

}  // namespace
}  // namespace swordfs::utils
