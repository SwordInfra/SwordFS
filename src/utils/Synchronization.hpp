// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/fibers/Baton.h>
#include <folly/fibers/TimedMutex.h>
#include <glog/logging.h>

#include <mutex>

#include "utils/ExecutionDomain.hpp"

namespace swordfs::utils {

// Synchronization for state owned by fiber-reachable business logic. In Debug
// builds it may only be acquired while a Folly fiber is actively executing.
class FiberMutex {
 public:
  void lock() {
    ExpectInFiberDomain();
    mutex_.lock();
  }

  bool try_lock() {
    ExpectInFiberDomain();
    return mutex_.try_lock();
  }

  void unlock() {
    ExpectInFiberDomain();
    mutex_.unlock();
  }

 private:
  folly::fibers::TimedMutex mutex_;
};

class FiberRWMutex {
 public:
  void lock() {
    ExpectInFiberDomain();
    mutex_.lock();
  }

  bool try_lock() {
    ExpectInFiberDomain();
    return mutex_.try_lock();
  }

  void unlock() {
    ExpectInFiberDomain();
    mutex_.unlock();
  }

  void lock_shared() {
    ExpectInFiberDomain();
    mutex_.lock_shared();
  }

  bool try_lock_shared() {
    ExpectInFiberDomain();
    return mutex_.try_lock_shared();
  }

  void unlock_shared() {
    ExpectInFiberDomain();
    mutex_.unlock_shared();
  }

 private:
  folly::fibers::TimedRWMutexWritePriority<folly::fibers::Baton> mutex_;
};

// Synchronization for POSIX-thread-only state. Fiber use is a programming
// error and is rejected in Debug builds.
class ThreadMutex {
 public:
  void lock() {
    ExpectInThreadDomain();
    mutex_.lock();
  }

  bool try_lock() {
    ExpectInThreadDomain();
    return mutex_.try_lock();
  }

  void unlock() {
    ExpectInThreadDomain();
    mutex_.unlock();
  }

 private:
  std::mutex mutex_;
};

// Baton is intentionally cross-domain: it is the hand-off primitive between
// fiber work and blocking-thread completion/drain paths.
using FiberBaton = folly::fibers::Baton;

}  // namespace swordfs::utils
