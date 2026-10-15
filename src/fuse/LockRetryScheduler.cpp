// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "fuse/LockRetryScheduler.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include "utils/ExecutionDomain.hpp"
#include "utils/FiberRuntime.hpp"

namespace swordfs::fuse {
namespace {

constexpr size_t kMaxWaiters = 1024;
constexpr size_t kMaxGroupsPerTick = 4;
constexpr size_t kMaxWakePerGroup = 8;
constexpr auto kProbePeriod = std::chrono::milliseconds(50);

uint64_t EndOfRange(const metadata::ILockAuthority::PosixRange &range) {
  return range.length == 0 ? std::numeric_limits<uint64_t>::max() : range.start + range.length;
}

bool RangeConflicts(const metadata::ILockAuthority::PosixRange &left,
                    const metadata::ILockAuthority::PosixRange &right) {
  return left.start < EndOfRange(right) && right.start < EndOfRange(left) &&
         (left.type == metadata::ILockAuthority::PosixType::kWrite ||
          right.type == metadata::ILockAuthority::PosixType::kWrite);
}

}  // namespace

utils::Status LockRetryScheduler::RegisterWaiter(const std::shared_ptr<Waiter> &waiter) {
  utils::ExpectInFiberDomain();
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  if (waiters_.size() >= kMaxWaiters) {
    return utils::Status::NoLocks("Mount advisory lock waiter admission limit reached");
  }
  waiters_.push_back(waiter);
  if (!coordinator_running_) {
    auto *runtime = utils::CurrentDriverFiberRuntime();
    if (runtime == nullptr || !runtime->Submit([this] { RunCoordinator(); })) {
      waiters_.pop_back();
      return utils::Status::Unavailable("cannot start lock retry coordinator");
    }
    coordinator_running_ = true;
  }
  return utils::Status::OK();
}

void LockRetryScheduler::RemoveWaiter(const std::shared_ptr<Waiter> &waiter) {
  utils::ExpectInFiberDomain();
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  waiter->active.store(false, std::memory_order_release);
  std::erase(waiters_, waiter);
}

bool LockRetryScheduler::Eligible(const Waiter &waiter,
                                  const std::vector<metadata::ILockAuthority::LockHolder> &holders) {
  for (const auto &holder : holders) {
    if (holder.local_generation && holder.kernel_owner == waiter.kernel_owner) {
      continue;
    }
    if (waiter.flock) {
      if (waiter.flock_type == FlockType::kExclusive || holder.flock == FlockType::kExclusive) {
        return false;
      }
      continue;
    }
    for (const auto &record : holder.ranges) {
      if (RangeConflicts(waiter.range, record)) {
        return false;
      }
    }
  }
  return true;
}

bool LockRetryScheduler::Compatible(const Waiter &left, const Waiter &right) {
  if (left.flock) {
    return left.flock_type == FlockType::kShared && right.flock_type == FlockType::kShared;
  }
  return !RangeConflicts(left.range, right.range);
}

void LockRetryScheduler::NotifyPotentialAvailability(const std::shared_ptr<Waiter> &waiter, int error) {
  // Cancellation and probe completion may race across distinct EventBases.
  // Atomic notification ownership and shared Baton storage prohibit a late
  // probe from accessing a retired request or producing a second wakeup.
  if (waiter->active.load(std::memory_order_acquire) && !waiter->notified.exchange(true, std::memory_order_acq_rel)) {
    waiter->probe_error.store(error, std::memory_order_release);
    waiter->baton.post();
  }
}

void LockRetryScheduler::ProbeWaiters() {
  utils::ExpectInFiberDomain();
  using Key = std::pair<uint64_t, bool>;
  std::map<Key, std::vector<std::shared_ptr<Waiter>>> groups;
  {
    std::lock_guard<utils::FiberMutex> lock(mutex_);
    for (const auto &waiter : waiters_) {
      if (waiter->active.load(std::memory_order_acquire) && !waiter->notified.load(std::memory_order_acquire)) {
        groups[{waiter->inode_ino, waiter->flock}].push_back(waiter);
      }
    }
  }
  if (groups.empty()) {
    return;
  }
  auto it = groups.begin();
  std::advance(it, next_group_ % groups.size());
  const size_t budget = std::min(kMaxGroupsPerTick, groups.size());
  next_group_ += budget;
  for (size_t i = 0; i < budget; ++i) {
    if (it == groups.end()) {
      it = groups.begin();
    }
    const auto [inode_ino, flock] = it->first;
    const auto waiters = it->second;
    ++it;
    std::vector<metadata::ILockAuthority::LockHolder> holders;
    // At most one background read-only Redis probe occupies the existing
    // metadata worker pool at a time. Never queue one attempt per waiter.
    auto status = executor_.RunFromFiber(
        [this, inode_ino, flock, &holders] { return authority_.Inspect(inode_ino, flock, &holders); });
    if (!status.ok()) {
      for (const auto &waiter : waiters) {
        NotifyPotentialAvailability(waiter, status.ToErrno());
      }
      continue;
    }
    std::vector<std::shared_ptr<Waiter>> selected;
    for (const auto &waiter : waiters) {
      if (!waiter->active.load(std::memory_order_acquire) || waiter->notified.load(std::memory_order_acquire) ||
          !Eligible(*waiter, holders)) {
        continue;
      }
      bool compatible = true;
      for (const auto &other : selected) {
        if (!Compatible(*waiter, *other)) {
          compatible = false;
          break;
        }
      }
      if (!compatible) {
        continue;
      }
      NotifyPotentialAvailability(waiter, 0);
      selected.push_back(waiter);
      if (selected.size() >= kMaxWakePerGroup) {
        break;
      }
    }
  }
}

void LockRetryScheduler::RunCoordinator() {
  utils::ExpectInFiberDomain();
  while (true) {
    {
      std::lock_guard<utils::FiberMutex> lock(mutex_);
      if (waiters_.empty()) {
        coordinator_running_ = false;
        return;
      }
    }
    ProbeWaiters();
    // This is one timer per Mount scheduler, not one per waiting request.
    // The coordinator owns no worker while sleeping; timer wakeups only
    // trigger read-only probes, never authority grant operations.
    utils::FiberBaton timer;
    timer.try_wait_for(kProbePeriod);
  }
}

}  // namespace swordfs::fuse
