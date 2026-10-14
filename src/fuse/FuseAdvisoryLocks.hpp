// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <fcntl.h>

#include <cstdint>

#include "metadata/LockAuthority.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/Status.hpp"

namespace swordfs::fuse {

// Single fiber-domain adapter for POSIX record and BSD flock locks. Both
// operations share the thread-only distributed authority and executor.
// Ownership comes from fi->lock_owner, not the file handle or diagnostic PID.
class FuseAdvisoryLocks {
 public:
  FuseAdvisoryLocks(metadata::ILockAuthority &authority, utils::BlockingExecutor &executor)
      : authority_(authority), executor_(executor) {
  }

  utils::Status SetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock);
  utils::Status GetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock, struct flock *result);
  utils::Status UnlockPosixOwner(uint64_t inode_ino, uint64_t kernel_owner);
  utils::Status Flock(uint64_t inode_ino, uint64_t kernel_owner, int op);
  utils::Status UnlockFlockOwner(uint64_t inode_ino, uint64_t kernel_owner);
  bool CanGrant() const;

 private:
  static utils::Status ToRange(const struct flock &lock, bool allow_unlock,
                               metadata::ILockAuthority::PosixRange *range);

 private:
  metadata::ILockAuthority &authority_;
  utils::BlockingExecutor &executor_;
};

}  // namespace swordfs::fuse
