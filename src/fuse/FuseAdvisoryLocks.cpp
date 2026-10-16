// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "fuse/FuseAdvisoryLocks.hpp"

#include <sys/file.h>

#include <cerrno>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "utils/ExecutionDomain.hpp"
#include "utils/FiberRuntime.hpp"

namespace swordfs::fuse {

utils::Status FuseAdvisoryLocks::ToRange(const struct flock &lock, bool allow_unlock,
                                         metadata::ILockAuthority::PosixRange *range) {
  if (lock.l_whence != SEEK_SET || lock.l_start < 0 || lock.l_len < 0 ||
      (lock.l_len != 0 && lock.l_start > std::numeric_limits<off_t>::max() - lock.l_len)) {
    return utils::Status::InvalidArgument("invalid normalized POSIX lock range");
  }
  metadata::ILockAuthority::PosixType type;
  switch (lock.l_type) {
    case F_RDLCK:
      type = metadata::ILockAuthority::PosixType::kRead;
      break;
    case F_WRLCK:
      type = metadata::ILockAuthority::PosixType::kWrite;
      break;
    case F_UNLCK:
      if (!allow_unlock) {
        return utils::Status::InvalidArgument("GETLK requires a read or write lock");
      }
      type = metadata::ILockAuthority::PosixType::kUnlock;
      break;
    default:
      return utils::Status::InvalidArgument("unknown POSIX lock type");
  }
  *range = {static_cast<uint64_t>(lock.l_start), static_cast<uint64_t>(lock.l_len), type, lock.l_pid};
  return utils::Status::OK();
}

utils::FiberMutex &FuseAdvisoryLocks::PosixOperationMutex(uint64_t inode_ino, uint64_t kernel_owner) {
  return posix_operation_mutexes_[(inode_ino ^ (kernel_owner * 0x9e3779b97f4a7c15ULL)) %
                                  posix_operation_mutexes_.size()];
}

utils::Status FuseAdvisoryLocks::SetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock,
                                          bool blocking, const folly::CancellationToken &token,
                                          std::optional<uint64_t> file_handle) {
  utils::ExpectInFiberDomain();
  metadata::ILockAuthority::PosixRange range{};
  auto status = ToRange(lock, true, &range);
  if (!status.ok()) {
    return status;
  }
  while (true) {
    if (blocking && token.isCancellationRequested()) {
      return utils::Status::Interrupted("cancelled POSIX lock wait");
    }
    {
      // Serialize the *actual authority mutation* with FLUSH for this Owner.
      // Hint counters alone cannot protect a late grant from an earlier
      // full-range unlock. Release this stripe before blocking on a waiter.
      std::lock_guard operation_lock(PosixOperationMutex(inode_ino, kernel_owner));
      if (range.type != metadata::ILockAuthority::PosixType::kUnlock) {
        // Unknown backend replies may have committed, so mark before I/O.
        std::lock_guard lock(posix_owner_mutex_);
        auto &hint = posix_owner_attempts_[inode_ino][kernel_owner];
        ++hint.attempt;
        if (file_handle.has_value()) {
          posix_handle_owners_[inode_ino][*file_handle][kernel_owner] = hint.attempt;
        }
      }
      status = executor_.RunFromFiber(
          [this, inode_ino, kernel_owner, range] { return authority_.SetPosix(inode_ino, kernel_owner, range); });
      if (status.ok() && range.type != metadata::ILockAuthority::PosixType::kUnlock) {
        std::lock_guard lock(posix_owner_mutex_);
        posix_owner_attempts_[inode_ino][kernel_owner].cleanup_pending = false;
      }
    }
    if (!blocking || range.type == metadata::ILockAuthority::PosixType::kUnlock || status.ToErrno() != EAGAIN) {
      return status;
    }
    status =
        WaitForAvailability(inode_ino, kernel_owner, false, range, metadata::ILockAuthority::FlockType::kUnlock, token);
    if (!status.ok()) {
      return status;
    }
  }
}

utils::Status FuseAdvisoryLocks::WaitForAvailability(uint64_t inode_ino, uint64_t kernel_owner, bool flock,
                                                     metadata::ILockAuthority::PosixRange range,
                                                     metadata::ILockAuthority::FlockType flock_type,
                                                     const folly::CancellationToken &token) {
  auto waiter = std::make_shared<LockRetryScheduler::Waiter>();
  waiter->inode_ino = inode_ino;
  waiter->kernel_owner = kernel_owner;
  waiter->flock = flock;
  waiter->range = range;
  waiter->flock_type = flock_type;
  if (token.isCancellationRequested()) {
    return utils::Status::Interrupted("cancelled lock wait");
  }
  auto status = scheduler_.RegisterWaiter(waiter);
  if (!status.ok()) {
    return status;
  }
  auto *runtime = utils::CurrentDriverFiberRuntime();
  // The cancellation framework owns the request token; this callback only
  // sends a thread-safe wake notification. The original Fiber unregisters
  // its own waiter and decides the EINTR-vs-completed-grant outcome.
  std::unique_ptr<folly::CancellationCallback> cancellation;
  if (token.canBeCancelled()) {
    cancellation = std::make_unique<folly::CancellationCallback>(token, [runtime, waiter] {
      const auto wake = [waiter] {
        if (!waiter->notified.exchange(true, std::memory_order_acq_rel)) {
          waiter->baton.post();
        }
      };
      if (runtime == nullptr || !runtime->Post(wake)) {
        wake();
      }
    });
  }
  waiter->baton.wait();
  scheduler_.RemoveWaiter(waiter);
  cancellation.reset();
  if (token.isCancellationRequested()) {
    return utils::Status::Interrupted("cancelled lock wait");
  }
  const int error = waiter->probe_error.load(std::memory_order_acquire);
  if (error == ENOLCK) {
    return utils::Status::NoLocks("lock inspection exhausted its owner budget");
  }
  if (error != 0) {
    return utils::Status::IOError("lock inspection failed before retry");
  }
  return utils::Status::OK();
}

utils::Status FuseAdvisoryLocks::GetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock,
                                          struct flock *result) {
  utils::ExpectInFiberDomain();
  if (result == nullptr) {
    return utils::Status::InvalidArgument("missing GETLK reply buffer");
  }
  metadata::ILockAuthority::PosixRange range{};
  auto status = ToRange(lock, false, &range);
  if (!status.ok()) {
    return status;
  }
  std::optional<metadata::ILockAuthority::PosixConflict> conflict;
  status = executor_.RunFromFiber([this, inode_ino, kernel_owner, range, &conflict] {
    return authority_.GetPosix(inode_ino, kernel_owner, range, &conflict);
  });
  if (!status.ok()) {
    return status;
  }
  *result = lock;
  if (!conflict) {
    result->l_type = F_UNLCK;
    result->l_pid = 0;
    return utils::Status::OK();
  }
  if (conflict->start > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
      conflict->length > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    return utils::Status::Range("conflicting POSIX lock cannot be represented by flock");
  }
  result->l_type = conflict->type == metadata::ILockAuthority::PosixType::kRead ? F_RDLCK : F_WRLCK;
  result->l_whence = SEEK_SET;
  result->l_start = static_cast<off_t>(conflict->start);
  result->l_len = static_cast<off_t>(conflict->length);
  result->l_pid = conflict->pid;
  return utils::Status::OK();
}

utils::Status FuseAdvisoryLocks::UnlockPosixOwner(uint64_t inode_ino, uint64_t kernel_owner) {
  return UnlockPosixOwnerIfCurrent(inode_ino, kernel_owner);
}

utils::Status FuseAdvisoryLocks::UnlockPosixOwnerIfCurrent(uint64_t inode_ino, uint64_t kernel_owner,
                                                           std::optional<uint64_t> expected_attempt,
                                                           bool pending_only) {
  utils::ExpectInFiberDomain();
  utils::Status status;
  {
    std::lock_guard operation_lock(PosixOperationMutex(inode_ino, kernel_owner));
    {
      std::lock_guard lock(posix_owner_mutex_);
      const auto inode_it = posix_owner_attempts_.find(inode_ino);
      if (inode_it == posix_owner_attempts_.end()) {
        return utils::Status::OK();
      }
      const auto owner_it = inode_it->second.find(kernel_owner);
      if (owner_it == inode_it->second.end() ||
          (expected_attempt.has_value() && owner_it->second.attempt != *expected_attempt) ||
          (pending_only && !owner_it->second.cleanup_pending)) {
        return utils::Status::OK();
      }
      owner_it->second.cleanup_pending = true;
    }
    status = executor_.RunFromFiber([this, inode_ino, kernel_owner] {
      return authority_.SetPosix(inode_ino, kernel_owner, {0, 0, metadata::ILockAuthority::PosixType::kUnlock});
    });
    if (status.ok()) {
      std::lock_guard lock(posix_owner_mutex_);
      const auto inode_it = posix_owner_attempts_.find(inode_ino);
      if (inode_it != posix_owner_attempts_.end()) {
        inode_it->second.erase(kernel_owner);
        if (inode_it->second.empty()) {
          posix_owner_attempts_.erase(inode_it);
        }
      }
    }
  }
  if (!status.ok()) {
    SchedulePosixCleanup();
  }
  return status;
}

utils::Status FuseAdvisoryLocks::ReleasePosixHandle(uint64_t inode_ino, uint64_t file_handle) {
  utils::ExpectInFiberDomain();
  std::unordered_map<uint64_t, uint64_t> owners;
  {
    std::lock_guard lock(posix_owner_mutex_);
    const auto inode_it = posix_handle_owners_.find(inode_ino);
    if (inode_it == posix_handle_owners_.end()) {
      return utils::Status::OK();
    }
    const auto handle_it = inode_it->second.find(file_handle);
    if (handle_it == inode_it->second.end()) {
      return utils::Status::OK();
    }
    owners = std::move(handle_it->second);
    inode_it->second.erase(handle_it);
    if (inode_it->second.empty()) {
      posix_handle_owners_.erase(inode_it);
    }
  }
  utils::Status first_error = utils::Status::OK();
  for (const auto &[owner, attempt] : owners) {
    auto status = UnlockPosixOwnerIfCurrent(inode_ino, owner, attempt);
    if (first_error.ok() && !status.ok()) {
      first_error = status;
    }
  }
  return first_error;
}

void FuseAdvisoryLocks::StopBackgroundCleanup() {
  cleanup_stopping_.store(true, std::memory_order_release);
}

void FuseAdvisoryLocks::SchedulePosixCleanup() {
  utils::ExpectInFiberDomain();
  if (cleanup_stopping_.load(std::memory_order_acquire) || !authority_.CanGrant()) {
    return;  // A terminal lease's TTL already bounds the stale record.
  }
  std::lock_guard lock(posix_owner_mutex_);
  if (cleanup_running_) {
    return;
  }
  auto *runtime = utils::CurrentDriverFiberRuntime();
  if (runtime != nullptr && runtime->Submit([this] { RetryPendingPosixCleanup(); })) {
    cleanup_running_ = true;
  }
}

void FuseAdvisoryLocks::RetryPendingPosixCleanup() {
  utils::ExpectInFiberDomain();
  while (!cleanup_stopping_.load(std::memory_order_acquire) && authority_.CanGrant()) {
    std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> pending;
    {
      std::lock_guard lock(posix_owner_mutex_);
      for (const auto &[inode_ino, owners] : posix_owner_attempts_) {
        for (const auto &[owner, hint] : owners) {
          if (hint.cleanup_pending) {
            pending.emplace_back(inode_ino, owner, hint.attempt);
          }
        }
      }
      if (pending.empty()) {
        cleanup_running_ = false;
        return;
      }
    }
    // One coordinator per Mount, not one Redis retry per closed file.
    utils::FiberBaton timer;
    timer.try_wait_for(std::chrono::milliseconds(250));
    for (const auto &[inode_ino, owner, attempt] : pending) {
      if (cleanup_stopping_.load(std::memory_order_acquire) || !authority_.CanGrant()) {
        break;
      }
      (void)UnlockPosixOwnerIfCurrent(inode_ino, owner, attempt, true);
    }
  }
  std::lock_guard lock(posix_owner_mutex_);
  cleanup_running_ = false;
}

bool FuseAdvisoryLocks::CanGrant() const {
  return authority_.CanGrant();
}

utils::Status FuseAdvisoryLocks::Flock(uint64_t inode_ino, uint64_t kernel_owner, int op,
                                       const folly::CancellationToken &token) {
  utils::ExpectInFiberDomain();
  metadata::ILockAuthority::FlockType type;
  switch (op & ~LOCK_NB) {
    case LOCK_SH:
      type = metadata::ILockAuthority::FlockType::kShared;
      break;
    case LOCK_EX:
      type = metadata::ILockAuthority::FlockType::kExclusive;
      break;
    case LOCK_UN:
      type = metadata::ILockAuthority::FlockType::kUnlock;
      break;
    default:
      return utils::Status::InvalidArgument("invalid BSD flock operation");
  }
  const bool blocking = type != metadata::ILockAuthority::FlockType::kUnlock && !(op & LOCK_NB);
  while (true) {
    if (blocking && token.isCancellationRequested()) {
      return utils::Status::Interrupted("cancelled flock wait");
    }
    auto status = executor_.RunFromFiber(
        [this, inode_ino, kernel_owner, type] { return authority_.SetFlock(inode_ino, kernel_owner, type); });
    if (!blocking || status.ToErrno() != EAGAIN) {
      return status;
    }
    status = WaitForAvailability(inode_ino, kernel_owner, true, {}, type, token);
    if (!status.ok()) {
      return status;
    }
  }
}

utils::Status FuseAdvisoryLocks::UnlockFlockOwner(uint64_t inode_ino, uint64_t kernel_owner) {
  utils::ExpectInFiberDomain();
  return executor_.RunFromFiber([this, inode_ino, kernel_owner] {
    return authority_.SetFlock(inode_ino, kernel_owner, metadata::ILockAuthority::FlockType::kUnlock);
  });
}

}  // namespace swordfs::fuse
