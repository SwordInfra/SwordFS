// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "fuse/FuseAdvisoryLocks.hpp"

#include <sys/file.h>

#include <limits>
#include <optional>

#include "utils/ExecutionDomain.hpp"

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

utils::Status FuseAdvisoryLocks::SetPosix(uint64_t inode_ino, uint64_t kernel_owner, const struct flock &lock) {
  utils::ExpectInFiberDomain();
  metadata::ILockAuthority::PosixRange range{};
  auto status = ToRange(lock, true, &range);
  if (!status.ok()) {
    return status;
  }
  return executor_.RunFromFiber(
      [this, inode_ino, kernel_owner, range] { return authority_.SetPosix(inode_ino, kernel_owner, range); });
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
  utils::ExpectInFiberDomain();
  return executor_.RunFromFiber([this, inode_ino, kernel_owner] {
    return authority_.SetPosix(inode_ino, kernel_owner, {0, 0, metadata::ILockAuthority::PosixType::kUnlock});
  });
}

bool FuseAdvisoryLocks::CanGrant() const {
  return authority_.CanGrant();
}

utils::Status FuseAdvisoryLocks::Flock(uint64_t inode_ino, uint64_t kernel_owner, int op) {
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
  if (type != metadata::ILockAuthority::FlockType::kUnlock && !(op & LOCK_NB)) {
    return utils::Status::OperationNotSupported("blocking BSD flock belongs to #433");
  }
  return executor_.RunFromFiber(
      [this, inode_ino, kernel_owner, type] { return authority_.SetFlock(inode_ino, kernel_owner, type); });
}

utils::Status FuseAdvisoryLocks::UnlockFlockOwner(uint64_t inode_ino, uint64_t kernel_owner) {
  utils::ExpectInFiberDomain();
  return executor_.RunFromFiber([this, inode_ino, kernel_owner] {
    return authority_.SetFlock(inode_ino, kernel_owner, metadata::ILockAuthority::FlockType::kUnlock);
  });
}

}  // namespace swordfs::fuse
