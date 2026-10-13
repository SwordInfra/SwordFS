// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <fcntl.h>

#include <cstdint>

#include "metadata/LockAuthority.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/Status.hpp"

namespace swordfs::fuse {

// Fiber-domain adapter from normalized libfuse flock ranges to the existing
// thread-only distributed authority. Ownership comes from fi->lock_owner,
// never the file handle or the diagnostic PID.
class PosixRecordLocks {
 public:
  PosixRecordLocks(metadata::ILockAuthority &authority, utils::BlockingExecutor &executor)
      : authority_(authority), executor_(executor) {
  }

  utils::Status Set(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock);
  utils::Status Get(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock, struct flock *result);
  utils::Status UnlockOwner(uint64_t inode_ino, uint64_t kernel_owner);
  bool CanGrant() const;

 private:
  static utils::Status ToRange(const struct flock &lock, bool allow_unlock,
                               metadata::ILockAuthority::PosixRange *range);

 private:
  metadata::ILockAuthority &authority_;
  utils::BlockingExecutor &executor_;
};

}  // namespace swordfs::fuse
