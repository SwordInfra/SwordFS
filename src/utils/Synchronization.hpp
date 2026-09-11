// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/fibers/Baton.h>
#include <folly/fibers/TimedMutex.h>
#include <glog/logging.h>

#include <mutex>

#include "utils/ExecutionDomain.hpp"

namespace swordfs::utils {

namespace detail {

inline void CheckFiberSynchronizationDomain() {
#ifndef NDEBUG
  DCHECK(CurrentExecutionDomain() != ExecutionDomain::kBlockingThread)
      << "fiber synchronization primitive used from blocking-thread domain";
#endif
}

inline void CheckThreadSynchronizationDomain() {
#ifndef NDEBUG
  DCHECK(CurrentExecutionDomain() != ExecutionDomain::kFiber)
      << "thread synchronization primitive used from fiber domain";
#endif
}

}  // namespace detail

// Synchronization for state owned by fiber-reachable business logic. Unknown
// context is allowed for initialization and direct unit tests; explicitly
// marked blocking-thread work is rejected in debug builds.
class FiberMutex {
 public:
  void lock() {
    detail::CheckFiberSynchronizationDomain();
    mutex_.lock();
  }

  bool try_lock() {
    detail::CheckFiberSynchronizationDomain();
    return mutex_.try_lock();
  }

  void unlock() {
    mutex_.unlock();
  }

 private:
  folly::fibers::TimedMutex mutex_;
};

class FiberRWMutex {
 public:
  void lock() {
    detail::CheckFiberSynchronizationDomain();
    mutex_.lock();
  }

  bool try_lock() {
    detail::CheckFiberSynchronizationDomain();
    return mutex_.try_lock();
  }

  void unlock() {
    mutex_.unlock();
  }

  void lock_shared() {
    detail::CheckFiberSynchronizationDomain();
    mutex_.lock_shared();
  }

  bool try_lock_shared() {
    detail::CheckFiberSynchronizationDomain();
    return mutex_.try_lock_shared();
  }

  void unlock_shared() {
    mutex_.unlock_shared();
  }

 private:
  folly::fibers::TimedRWMutexWritePriority<folly::fibers::Baton> mutex_;
};

// Synchronization for blocking/POSIX-thread-only state. Fiber use is a
// programming error and is rejected in debug builds.
class ThreadMutex {
 public:
  void lock() {
    detail::CheckThreadSynchronizationDomain();
    mutex_.lock();
  }

  bool try_lock() {
    detail::CheckThreadSynchronizationDomain();
    return mutex_.try_lock();
  }

  void unlock() {
    mutex_.unlock();
  }

 private:
  std::mutex mutex_;
};

// Baton is intentionally cross-domain: it is the hand-off primitive between
// fiber work and blocking-thread completion/drain paths.
using FiberBaton = folly::fibers::Baton;

}  // namespace swordfs::utils
