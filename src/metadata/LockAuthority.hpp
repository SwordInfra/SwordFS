// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>

#include "utils/Status.hpp"

namespace swordfs::metadata {

// The backend-independent, thread-domain lock contract consumed by the
// forthcoming #431/#432 VFS hooks. Only Redis implements this contract: it
// deliberately does not expose Redis keys, schemas, or a generic session.
class ILockAuthority {
 public:
  enum class PosixType { kRead, kWrite, kUnlock };
  struct PosixRange {
    uint64_t start;
    uint64_t length;  // zero means through EOF
    PosixType type;
    int32_t pid = 0;  // Kernel-supplied process PID; never used as owner identity.
  };
  struct PosixConflict {
    uint64_t start;
    uint64_t length;
    uint64_t kernel_owner;
    PosixType type;
    int32_t pid = 0;
  };
  enum class FlockType { kShared, kExclusive, kUnlock };

  virtual ~ILockAuthority() = default;

  virtual utils::Status SetPosix(uint64_t inode_ino, uint64_t kernel_owner, PosixRange request) = 0;
  virtual utils::Status GetPosix(uint64_t inode_ino, uint64_t kernel_owner, PosixRange request,
                                 std::optional<PosixConflict> *conflict) = 0;
  virtual utils::Status SetFlock(uint64_t inode_ino, uint64_t kernel_owner, FlockType type) = 0;

  // Fiber-safe, nonblocking pre-reply gate. A successful Redis mutation is
  // not a grant if its mount generation became invalid before FUSE replied.
  virtual bool CanGrant() const = 0;
};

}  // namespace swordfs::metadata
