// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisOwnerLease.hpp"

#include <folly/Random.h>
#include <folly/logging/xlog.h>
#include <time.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "metadata/redis/RedisKvTxn.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

namespace {

using namespace std::chrono_literals;

// Budget each command's pool, connect and socket waits separately. Multiple
// Redis commands (e.g. WATCH/GET/EXEC/SET) do NOT share an absolute deadline;
// the opt-in setting is intentionally named an I/O budget, not an end-to-end
// lease operation timeout.
std::chrono::milliseconds AtLeastOneMillisecond(std::chrono::milliseconds duration) {
  return std::max(duration, 1ms);
}

}  // namespace

RedisOwnerLease::RedisOwnerLease(const RedisMetaConfig &config, std::string_view volume_name)
    : RedisOwnerLease(config, volume_name, Timing{}) {
}

RedisOwnerLease::RedisOwnerLease(const RedisMetaConfig &config, std::string_view volume_name, Timing timing,
                                 ClockFn clock)
    : client_(BoundConfig(config, timing)),
      key_(config.db, volume_name),
      timing_(timing),
      clock_(clock ? std::move(clock) : ClockBoottime) {
  utils::ExpectInThreadDomain();
}

RedisOwnerLease::~RedisOwnerLease() {
  utils::ExpectInThreadDomain();
  (void)Revoke();
}

std::chrono::nanoseconds RedisOwnerLease::ClockBoottime() {
  timespec ts{};
  if (::clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
    // Never manufacture a valid lease on an untrustworthy local clock.
    return std::chrono::nanoseconds::max();
  }
  return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}

RedisMetaConfig RedisOwnerLease::BoundConfig(RedisMetaConfig config, const Timing &timing) {
  const auto slice = AtLeastOneMillisecond(timing.io_timeout / 3);
  config.connect_timeout = std::min(config.connect_timeout, slice);
  config.socket_timeout = std::min(config.socket_timeout, slice);
  config.pool_wait_timeout = std::min(config.pool_wait_timeout, slice);
  config.retry_attempts = 1;
  // This lease performs no concurrent Redis I/O after registration. One
  // pooled connection is enough, without multiplying pool-wait contention.
  config.pool_size = 1;
  return config;
}

utils::Status RedisOwnerLease::ValidateTiming() const {
  // Reserve at least one per-command I/O budget plus a renewal period
  // before local cutoff. The Redis socket API does not provide an absolute
  // multi-command deadline; the last confirmation is still checked below.
  if (timing_.ttl <= 0ms || timing_.ttl > 24h || timing_.safety_margin <= 0ms || timing_.safety_margin >= timing_.ttl ||
      timing_.renew_interval <= 0ms || timing_.io_timeout <= 0ms || timing_.io_timeout > 5s ||
      timing_.io_timeout >= timing_.ttl - timing_.safety_margin ||
      timing_.renew_interval >= timing_.ttl - timing_.safety_margin - timing_.io_timeout) {
    return utils::Status::InvalidArgument("invalid lock owner lease timings");
  }
  return utils::Status::OK();
}

utils::Status RedisOwnerLease::AllocateGeneration(uint64_t *generation) {
  // A missing or corrupt counter is *not* an invitation to restart history.
  // WATCH protects the validated value against concurrent allocation; the
  // native MULTI/INCRBY/EXEC commit is the only generation write.
  return client_.Transact([&](RedisKvTxn &txn) {
    std::string encoded;
    auto status = txn.Get(key_.OwnerGeneration(), &encoded);
    if (status.IsNotFound()) {
      return utils::Status::Unavailable("lock owner generation counter is missing; reformat required");
    }
    if (!status.ok()) {
      return status;
    }
    uint64_t previous = 0;
    const auto [end, error] = std::from_chars(encoded.data(), encoded.data() + encoded.size(), previous);
    if (error != std::errc{} || end != encoded.data() + encoded.size() || encoded != std::to_string(previous) ||
        previous >= static_cast<uint64_t>(INT64_MAX)) {
      return utils::Status::Malformed("lock owner generation counter is corrupt or exhausted");
    }
    *generation = previous + 1;
    return txn.IncrBy(key_.OwnerGeneration(), 1);
  });
}

utils::Status RedisOwnerLease::Register() {
  utils::ExpectInThreadDomain();
  std::lock_guard<utils::ThreadMutex> lock(mutex_);
  if (state_.load(std::memory_order_acquire) != State::kUnregistered) {
    return utils::Status::Unavailable("lock owner registration is one-shot");
  }
  auto status = ValidateTiming();
  if (!status.ok()) {
    state_.store(State::kTerminal, std::memory_order_release);
    return status;
  }

  uint64_t next_generation = 0;
  status = AllocateGeneration(&next_generation);
  if (!status.ok()) {
    state_.store(State::kTerminal, std::memory_order_release);
    return status;
  }

  // The time bound begins *before* the command is dispatched; a delayed
  // acknowledgement can never extend confidence from reply arrival time.
  const auto send_time = clock_().count();
  const auto lifetime_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(timing_.ttl - timing_.safety_margin).count();
  if (send_time < 0 || send_time > std::numeric_limits<int64_t>::max() - lifetime_ns) {
    state_.store(State::kTerminal, std::memory_order_release);
    return utils::Status::Unavailable("owner lease boot clock is invalid");
  }
  const auto nonce = std::to_string(folly::Random::rand64());
  status = client_.CreateOwnerLease(key_.OwnerLease(next_generation), nonce, timing_.ttl);
  const auto cutoff = send_time + lifetime_ns;
  if (!status.ok() || clock_().count() >= cutoff) {
    state_.store(State::kTerminal, std::memory_order_release);
    return status.ok() ? utils::Status::Unavailable("owner lease registration confirmation arrived too late") : status;
  }

  generation_.store(next_generation, std::memory_order_release);
  cutoff_ns_.store(cutoff, std::memory_order_release);
  state_.store(State::kActive, std::memory_order_release);
  try {
    heartbeat_ = std::thread([this] { Heartbeat(); });
  } catch (const std::system_error &error) {
    state_.store(State::kTerminal, std::memory_order_release);
    // No heartbeat was started; the exact-generation key can be revoked
    // synchronously without racing a late renewal.
    (void)client_.RevokeOwnerLease(key_.OwnerLease(next_generation));
    revoke_attempted_ = true;
    return utils::Status::Unavailable("owner lease heartbeat unavailable: " + std::string(error.what()));
  }
  return utils::Status::OK();
}

bool RedisOwnerLease::CanGrant(uint64_t generation) const {
  if (generation == 0 || generation != generation_.load(std::memory_order_acquire) ||
      state_.load(std::memory_order_acquire) != State::kActive) {
    return false;
  }
  const auto now_ns = clock_().count();
  // An invalid boot-clock sample must never authorize a lock grant, even
  // when the previous confirmed lease cutoff is still in the future.
  if (now_ns < 0 || now_ns >= cutoff_ns_.load(std::memory_order_acquire)) {
    state_.store(State::kTerminal, std::memory_order_release);
    return false;
  }
  return state_.load(std::memory_order_acquire) == State::kActive;
}

utils::Status RedisOwnerLease::Renew() {
  utils::ExpectInThreadDomain();
  std::lock_guard<utils::ThreadMutex> lock(mutex_);
  const auto generation = generation_.load(std::memory_order_acquire);
  if (!CanGrant(generation)) {
    return utils::Status::Unavailable("lock owner lease is terminal");
  }
  const auto send_time = clock_().count();
  const auto lifetime_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(timing_.ttl - timing_.safety_margin).count();
  if (send_time < 0 || send_time > std::numeric_limits<int64_t>::max() - lifetime_ns) {
    state_.store(State::kTerminal, std::memory_order_release);
    return utils::Status::Unavailable("owner lease boot clock is invalid");
  }
  auto status = client_.RenewOwnerLease(key_.OwnerLease(generation), timing_.ttl);
  if (status.IsOutcomeUnknown()) {
    // Transient ambiguity cannot extend the locally confirmed deadline.
    if (!CanGrant(generation)) {
      return utils::Status::Unavailable("lock owner lease expired during uncertain renewal");
    }
    return status;
  }
  if (!status.ok() || !CanGrant(generation)) {
    state_.store(State::kTerminal, std::memory_order_release);
    return status.ok() ? utils::Status::Unavailable("owner renewal confirmed after the local deadline") : status;
  }
  const auto cutoff = send_time + lifetime_ns;
  if (clock_().count() >= cutoff) {
    state_.store(State::kTerminal, std::memory_order_release);
    return utils::Status::Unavailable("owner renewal confirmation arrived too late");
  }
  cutoff_ns_.store(cutoff, std::memory_order_release);
  return utils::Status::OK();
}

void RedisOwnerLease::Heartbeat() {
  utils::ExpectInThreadDomain();
  while (!stop_signal_.try_acquire_for(timing_.renew_interval)) {
    if (state_.load(std::memory_order_acquire) != State::kActive) {
      break;
    }
    const auto status = Renew();
    if (!status.ok() && state_.load(std::memory_order_acquire) == State::kTerminal) {
      SWORDFS_LOG_WARN << "lock owner lease became unavailable: " << status.message();
      break;
    }
  }
}

utils::Status RedisOwnerLease::Revoke() {
  utils::ExpectInThreadDomain();
  // Only one caller may own the heartbeat join and exact-generation DEL.
  std::lock_guard<utils::ThreadMutex> revoke_lock(revoke_mutex_);
  {
    // Register also holds mutex_ until ACTIVE is published and the worker
    // exists. Never let Register overwrite a prior terminal transition.
    std::lock_guard<utils::ThreadMutex> lock(mutex_);
    state_.store(State::kTerminal, std::memory_order_release);
  }
  if (heartbeat_.joinable()) {
    stop_signal_.release();
    heartbeat_.join();  // No renewal can run after DEL.
  }
  std::lock_guard<utils::ThreadMutex> lock(mutex_);
  if (revoke_attempted_ || generation_.load(std::memory_order_acquire) == 0) {
    return utils::Status::OK();
  }
  revoke_attempted_ = true;
  auto status = client_.RevokeOwnerLease(key_.OwnerLease(generation_.load(std::memory_order_acquire)));
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "lock owner lease DEL was not confirmed; Redis TTL is fallback: " << status.message();
  }
  return status;
}

}  // namespace swordfs::metadata
