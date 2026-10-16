// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <fcntl.h>
#include <folly/CancellationToken.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fuse/LockRetryScheduler.hpp"
#include "metadata/LockAuthority.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::fuse {

// Single fiber-domain adapter for POSIX record and BSD flock locks. Both
// operations share the thread-only distributed authority and executor.
// Ownership comes from fi->lock_owner, not the file handle or diagnostic PID.
class FuseAdvisoryLocks {
 public:
  FuseAdvisoryLocks(metadata::ILockAuthority &authority, utils::BlockingExecutor &executor)
      : authority_(authority), executor_(executor), scheduler_(authority, executor) {
  }

  utils::Status SetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock, bool blocking = false,
                         const folly::CancellationToken &token = {}, std::optional<uint64_t> file_handle = {});
  utils::Status GetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock, struct flock *result);
  utils::Status UnlockPosixOwner(uint64_t inode_ino, uint64_t kernel_owner);
  // Last-close fallback for Owners whose requests never had a matching
  // FLUSH Owner. This does not implement/advertise OFD record-lock semantics.
  utils::Status ReleasePosixHandle(uint64_t inode_ino, uint64_t file_handle);
  // Called before the FUSE fiber runtime is drained on user-requested unmount.
  void StopBackgroundCleanup();
  utils::Status Flock(uint64_t inode_ino, uint64_t kernel_owner, int op, const folly::CancellationToken &token = {});
  utils::Status UnlockFlockOwner(uint64_t inode_ino, uint64_t kernel_owner);
  bool CanGrant() const;

 private:
  static utils::Status ToRange(const struct flock &lock, bool allow_unlock,
                               metadata::ILockAuthority::PosixRange *range);

 private:
  utils::Status WaitForAvailability(uint64_t inode_ino, uint64_t kernel_owner, bool flock,
                                    metadata::ILockAuthority::PosixRange range,
                                    metadata::ILockAuthority::FlockType flock_type,
                                    const folly::CancellationToken &token);
  utils::FiberMutex &PosixOperationMutex(uint64_t inode_ino, uint64_t kernel_owner);
  utils::Status UnlockPosixOwnerIfCurrent(uint64_t inode_ino, uint64_t kernel_owner,
                                          std::optional<uint64_t> expected_attempt = {}, bool pending_only = false);
  void SchedulePosixCleanup();
  void RetryPendingPosixCleanup();

 private:
  metadata::ILockAuthority &authority_;
  utils::BlockingExecutor &executor_;
  LockRetryScheduler scheduler_;

  // Only owners that attempted a POSIX grant may need an on-close remote
  // cleanup. In particular, ordinary FLUSH must not make Redis requests.
  // Record before the authority call: an unconfirmed mutation may have
  // committed. The attempt counter prevents a concurrent new grant from
  // being forgotten after an earlier FLUSH finishes its backend unlock.
  struct PosixOwnerHint {
    uint64_t attempt = 0;
    bool cleanup_pending = false;
  };
  // Stripe only the *mutating* remote calls for one Owner; never hold a
  // stripe while waiting on F_SETLKW's read-only retry scheduler.
  std::array<utils::FiberMutex, 64> posix_operation_mutexes_;
  utils::FiberMutex posix_owner_mutex_;
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, PosixOwnerHint>> posix_owner_attempts_;
  // Only handles that attempted a record lock are tracked. On final RELEASE,
  // cleanup only if the Owner's attempt is still current (no new grant).
  std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>>>
      posix_handle_owners_;
  std::atomic<bool> cleanup_stopping_{false};
  bool cleanup_running_ = false;  // Protected by posix_owner_mutex_.
};

}  // namespace swordfs::fuse
