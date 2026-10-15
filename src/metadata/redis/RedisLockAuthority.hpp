// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "metadata/LockAuthority.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisOwnerLease.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {

// Thread-domain Redis advisory-lock authority. #431/#432 own FUSE dispatch,
// POSIX close and flock last-close; #433 owns blocking waits/activation.
class RedisLockAuthority final : public ILockAuthority {
 public:
  RedisLockAuthority(const RedisMetaConfig &config, std::string_view volume_name, RedisOwnerLease &owner_lease);

  utils::Status SetPosix(uint64_t inode_ino, uint64_t kernel_owner, PosixRange request) override;
  utils::Status GetPosix(uint64_t inode_ino, uint64_t kernel_owner, PosixRange request,
                         std::optional<PosixConflict> *conflict) override;
  utils::Status SetFlock(uint64_t inode_ino, uint64_t kernel_owner, FlockType type) override;
  bool CanGrant() const override;
  utils::Status Inspect(uint64_t inode_ino, bool flock, std::vector<LockHolder> *holders) override;

 private:
  utils::Status CheckLease(uint64_t generation) const;
  utils::ThreadMutex &OwnerMutex(uint64_t inode_ino, uint64_t kernel_owner, bool flock);
  // Best effort, bounded and independent of the grant transaction.
  void CleanupStale(std::string_view hash);

 private:
  RedisMetaClient client_;
  redis::RedisKey key_;
  RedisOwnerLease &owner_lease_;
  std::array<utils::ThreadMutex, 128> mutexes_;
  // Opportunistic GC is off the ordinary lock path. Per-Hash scan cursors
  // continue across calls, but the local cache must not grow with the number
  // of inodes visited by a long-lived mount.
  std::array<utils::ThreadMutex, 64> gc_mutexes_;
  utils::ThreadMutex gc_cursor_mutex_;
  std::unordered_map<std::string, uint64_t> gc_cursors_;
};

}  // namespace swordfs::metadata
