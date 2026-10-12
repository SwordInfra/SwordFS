// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <semaphore>
#include <string_view>
#include <thread>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {

// Per-mount advisory-lock owner identity and liveness, separate from ordinary
// metadata I/O. Every instance registers at most one generation. Lease loss
// is terminal for this mount incarnation: callers may keep using FUSE for
// non-lock operations, but cannot report new successful lock grants.
class RedisOwnerLease {
 public:
  struct Timing {
    std::chrono::milliseconds ttl = std::chrono::seconds(60);
    std::chrono::milliseconds renew_interval = std::chrono::seconds(10);
    std::chrono::milliseconds safety_margin = std::chrono::seconds(15);
    std::chrono::milliseconds io_timeout = std::chrono::seconds(5);
  };

  using ClockFn = std::function<std::chrono::nanoseconds()>;

  RedisOwnerLease(const RedisMetaConfig &config, std::string_view volume_name);
  RedisOwnerLease(const RedisMetaConfig &config, std::string_view volume_name, Timing timing, ClockFn clock = {});
  ~RedisOwnerLease();

  RedisOwnerLease(const RedisOwnerLease &) = delete;
  RedisOwnerLease &operator=(const RedisOwnerLease &) = delete;

  // Thread/control-domain only: registration includes atomic counter
  // validation/allocation and a one-shot SET NX PX for its unique generation.
  utils::Status Register();
  // One native PEXPIRE XX. Unknown replies never extend local confidence.
  utils::Status Renew();
  // Serialize with Register, stop/join renewal, then exact-generation DEL.
  // Concurrent Revoke calls are idempotent; Redis errors fall back to TTL.
  utils::Status Revoke();

  // Fiber-safe, nonblocking gate for lock-request admission and successful
  // grant replies. #430 supplies the server-authoritative grant predicate.
  bool CanGrant(uint64_t generation) const;
  uint64_t generation() const {
    return generation_.load(std::memory_order_acquire);
  }

 private:
  enum class State { kUnregistered, kActive, kTerminal };

  static std::chrono::nanoseconds ClockBoottime();
  static RedisMetaConfig BoundConfig(RedisMetaConfig config, const Timing &timing);
  utils::Status ValidateTiming() const;
  utils::Status AllocateGeneration(uint64_t *generation);
  void Heartbeat();

 private:
  // Revoke callers serialize their join/DEL sequence. The inner mutex
  // also prevents an in-flight Register from publishing ACTIVE after revoke.
  utils::ThreadMutex revoke_mutex_;
  utils::ThreadMutex mutex_;
  RedisMetaClient client_;
  redis::RedisKey key_;
  Timing timing_;
  ClockFn clock_;
  mutable std::atomic<State> state_{State::kUnregistered};
  std::atomic<uint64_t> generation_{0};
  std::atomic<int64_t> cutoff_ns_{0};
  bool revoke_attempted_ = false;
  std::binary_semaphore stop_signal_{0};
  std::thread heartbeat_;
};

}  // namespace swordfs::metadata
