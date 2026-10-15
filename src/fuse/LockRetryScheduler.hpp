// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "metadata/LockAuthority.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::fuse {

// One logical Mount-local admission and grouped read-only probe coordinator.
// Only the originating lock-request fiber may perform a mutating grant.
class LockRetryScheduler {
 public:
  using PosixRange = metadata::ILockAuthority::PosixRange;
  using FlockType = metadata::ILockAuthority::FlockType;

  // gcov attributes an uncalled compiler-generated special member to this
  // declaration line. All executable waiter state/admission paths remain
  // instrumented; tests exercise default construction and Baton lifetime.
  struct Waiter {  // LCOV_EXCL_LINE
    uint64_t inode_ino;
    uint64_t kernel_owner;
    bool flock;
    PosixRange range{};
    FlockType flock_type = FlockType::kUnlock;
    utils::FiberBaton baton;
    std::atomic<bool> active{true};
    std::atomic<bool> notified{false};
    std::atomic<int> probe_error{0};
  };

  LockRetryScheduler(metadata::ILockAuthority &authority, utils::BlockingExecutor &executor)
      : authority_(authority), executor_(executor) {
  }

  utils::Status RegisterWaiter(const std::shared_ptr<Waiter> &waiter);
  void RemoveWaiter(const std::shared_ptr<Waiter> &waiter);

 private:
  void RunCoordinator();
  void ProbeWaiters();
  static bool Eligible(const Waiter &waiter, const std::vector<metadata::ILockAuthority::LockHolder> &holders);
  static bool Compatible(const Waiter &left, const Waiter &right);
  static void NotifyPotentialAvailability(const std::shared_ptr<Waiter> &waiter, int error);

 private:
  metadata::ILockAuthority &authority_;
  utils::BlockingExecutor &executor_;
  utils::FiberMutex mutex_;
  std::vector<std::shared_ptr<Waiter>> waiters_;
  bool coordinator_running_ = false;
  size_t next_group_ = 0;
};

}  // namespace swordfs::fuse
