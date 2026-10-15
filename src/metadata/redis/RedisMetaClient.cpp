// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaClient.hpp"

#include <folly/Random.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <thread>

#include "metadata/redis/RedisKvTxn.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {
namespace {

constexpr auto kMaxRetryBackoff = std::chrono::milliseconds(1000);
constexpr std::string_view kIncrNonNegativeScript = R"lua(
local current = redis.call('GET', KEYS[1])
if current and string.sub(current, 1, 1) == '-' then
  return redis.error_reply('ERR counter must be non-negative')
end
return redis.call('INCR', KEYS[1])
)lua";

constexpr std::string_view kIncrExistingScript = R"lua(
local current = redis.call('GET', KEYS[1])
if not current or not string.match(current, '^%d+$') then
  return redis.error_reply('ERR existing non-negative counter required')
end
return redis.call('INCR', KEYS[1])
)lua";

constexpr std::string_view kCreateListScript = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then
  redis.call('RPUSH', KEYS[1], ARGV[1])
  return 1
end
if kind ~= 'list' then return -2 end
if redis.call('LINDEX', KEYS[1], 0) ~= ARGV[1] then return -3 end
if redis.call('LLEN', KEYS[1]) ~= 1 then return -4 end
return 1
)lua";

constexpr std::string_view kAppendListScript = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return -1 end
if kind ~= 'list' then return -2 end
if redis.call('LINDEX', KEYS[1], 0) ~= ARGV[1] then return -3 end
return redis.call('RPUSH', KEYS[1], ARGV[2])
)lua";

sw::redis::ConnectionOptions MakeConnectionOptions(const RedisMetaConfig &config) {
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  options.db = config.db;
  options.connect_timeout = config.connect_timeout;
  options.socket_timeout = config.socket_timeout;
  if (config.username.has_value()) {
    options.user = *config.username;
  }
  if (config.password.has_value()) {
    options.password = *config.password;
  }
  return options;
}

void Backoff(int attempt, std::chrono::milliseconds base_delay) {
  const int exponent = std::min(attempt, 10);
  const int multiplier = 1 << exponent;
  const auto max_base_delay = kMaxRetryBackoff / multiplier;
  const auto max_delay = std::min(base_delay, max_base_delay) * multiplier;
  const auto delay = std::chrono::milliseconds(folly::Random::rand64(max_delay.count() + 1));
  std::this_thread::sleep_for(delay);
}

utils::Status RedisError(const char *operation, const sw::redis::Error &error) {
  return utils::Status::IOError("Redis " + std::string(operation) + " failed: " + error.what());
}

utils::Status RedisUnavailable(const char *operation, const sw::redis::Error &error) {
  return utils::Status::Unavailable("Redis " + std::string(operation) + " unavailable: " + error.what());
}

template <typename Fn>
utils::Status RunReadOnlyCommand(const char *operation, Fn &&fn) {
  try {
    return fn();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError(operation, error);
  } catch (const sw::redis::Error &error) {
    return RedisUnavailable(operation, error);
  }
}

void ValidateConfig(const RedisMetaConfig &config) {
  if (config.connect_timeout <= std::chrono::milliseconds(0)) {
    throw std::invalid_argument("connect_timeout must be positive");
  }
  if (config.socket_timeout <= std::chrono::milliseconds(0)) {
    throw std::invalid_argument("socket_timeout must be positive");
  }
  if (config.pool_size == 0) {
    throw std::invalid_argument("pool_size must be positive");
  }
  if (config.pool_wait_timeout <= std::chrono::milliseconds(0)) {
    throw std::invalid_argument("pool_wait_timeout must be positive");
  }
  if (config.retry_attempts <= 0) {
    throw std::invalid_argument("retry_attempts must be positive");
  }
  if (config.retry_backoff < std::chrono::milliseconds(0)) {
    throw std::invalid_argument("retry_backoff must not be negative");
  }
}

}  // namespace

RedisMetaClient::RedisMetaClient(const RedisMetaConfig &config)
    : retry_attempts_(config.retry_attempts), retry_backoff_(config.retry_backoff) {
  utils::ExpectInThreadDomain();
  ValidateConfig(config);
  sw::redis::ConnectionPoolOptions pool_options;
  pool_options.size = config.pool_size;
  pool_options.wait_timeout = config.pool_wait_timeout;
  redis_ = std::make_unique<sw::redis::Redis>(MakeConnectionOptions(config), pool_options);
}

RedisMetaClient::~RedisMetaClient() {
  utils::ExpectInThreadDomain();
}

utils::Status RedisMetaClient::Ping() {
  utils::ExpectInThreadDomain();
  return RunReadOnlyCommand("PING", [&] {
    redis_->ping();
    return utils::Status::OK();
  });
}

utils::Status RedisMetaClient::Get(std::string_view key, std::string *value) {
  utils::ExpectInThreadDomain();
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  return RunReadOnlyCommand("GET", [&] {
    auto result = redis_->get(std::string(key));
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis key not found");
    }
    *value = std::move(*result);
    return utils::Status::OK();
  });
}

utils::Status RedisMetaClient::MGet(const std::vector<std::string> &keys,
                                    std::vector<std::optional<std::string>> *values) {
  utils::ExpectInThreadDomain();
  if (values == nullptr) {
    return utils::Status::InvalidArgument("Redis MGET output is null");
  }
  values->clear();
  if (keys.empty()) {
    return utils::Status::OK();
  }
  return RunReadOnlyCommand("MGET", [&] {
    redis_->mget(keys.begin(), keys.end(), std::back_inserter(*values));
    return utils::Status::OK();
  });
}

utils::Status RedisMetaClient::HScan(std::string_view key, uint64_t cursor, size_t count,
                                     std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor) {
  utils::ExpectInThreadDomain();
  if (values == nullptr || next_cursor == nullptr) {
    return utils::Status::InvalidArgument("Redis HSCAN output is null");
  }
  return RunReadOnlyCommand("HSCAN", [&] {
    values->clear();
    *next_cursor = cursor;
    *next_cursor =
        redis_->hscan(std::string(key), *next_cursor, static_cast<long long>(count), std::back_inserter(*values));
    return utils::Status::OK();
  });
}

utils::Status RedisMetaClient::Incr(std::string_view key, uint64_t *value) {
  utils::ExpectInThreadDomain();
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis INCR output is null");
  }
  try {
    *value = redis_->incr(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    // A valid Redis error reply is a known command result, so the caller does
    // not need to reconcile a possibly lost allocation identity.
    return RedisError("INCR", error);
  } catch (const sw::redis::Error &error) {
    // redis++ combines command send and acknowledgement receive in one call.
    // An opaque I/O/protocol failure cannot prove whether the counter advanced.
    return utils::Status::OutcomeUnknown("Redis INCR outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::IncrNonNegative(std::string_view key, uint64_t *value) {
  utils::ExpectInThreadDomain();
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis validated INCR output is null");
  }
  try {
    const auto result = redis_->eval<long long>(kIncrNonNegativeScript, {key}, {});
    *value = static_cast<uint64_t>(result);
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("validated INCR", error);
  } catch (const sw::redis::Error &error) {
    return utils::Status::OutcomeUnknown("Redis validated INCR outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::IncrExistingNonNegative(std::string_view key, uint64_t *value) {
  utils::ExpectInThreadDomain();
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis validated INCR output is null");
  }
  try {
    const auto result = redis_->eval<long long>(kIncrExistingScript, {key}, {});
    *value = static_cast<uint64_t>(result);
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("existing INCR", error);
  } catch (const sw::redis::Error &error) {
    return utils::Status::OutcomeUnknown("Redis existing INCR outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::LRange(std::string_view key, std::vector<std::string> *values) {
  utils::ExpectInThreadDomain();
  if (values == nullptr) {
    return utils::Status::InvalidArgument("Redis LRANGE output is null");
  }
  return RunReadOnlyCommand("LRANGE", [&] {
    std::vector<std::string> loaded;
    redis_->lrange(std::string(key), 0, -1, std::back_inserter(loaded));
    *values = std::move(loaded);
    return utils::Status::OK();
  });
}

utils::Status RedisMetaClient::CreateListWithHeader(std::string_view key, std::string_view header) {
  utils::ExpectInThreadDomain();
  try {
    const auto result = redis_->eval<long long>(kCreateListScript, {key}, {header});
    if (result == -4) {
      return utils::Status::AlreadyExists("Redis index already contains committed records");
    }
    if (result != 1) {
      return utils::Status::Malformed("Redis index header or type mismatch");
    }
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("LIST create", error);
  } catch (const sw::redis::Error &error) {
    return utils::Status::OutcomeUnknown("Redis LIST create outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::AppendToListWithHeader(std::string_view key, std::string_view header,
                                                      std::string_view value) {
  utils::ExpectInThreadDomain();
  try {
    const auto result = redis_->eval<long long>(kAppendListScript, {key}, {header, value});
    if (result == -1) {
      return utils::Status::NotFound("Redis Slice index is missing");
    }
    if (result < 0) {
      return utils::Status::Malformed("Redis index header or type mismatch");
    }
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("LIST append", error);
  } catch (const sw::redis::Error &error) {
    // RPUSH is non-idempotent; a lost response must never cause blind replay.
    return utils::Status::OutcomeUnknown("Redis LIST append outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::CreateOwnerLease(std::string_view key, std::string_view nonce,
                                                std::chrono::milliseconds ttl) {
  utils::ExpectInThreadDomain();
  if (ttl <= std::chrono::milliseconds::zero()) {
    return utils::Status::InvalidArgument("owner lease TTL must be positive");
  }
  try {
    if (!redis_->set(std::string(key), std::string(nonce), ttl, sw::redis::UpdateType::NOT_EXIST)) {
      return utils::Status::AlreadyExists("owner lease generation key unexpectedly exists");
    }
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("owner SET NX PX", error);
  } catch (const sw::redis::Error &error) {
    return utils::Status::OutcomeUnknown("Redis owner SET NX PX outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::RenewOwnerLease(std::string_view key, std::chrono::milliseconds ttl) {
  utils::ExpectInThreadDomain();
  if (ttl <= std::chrono::milliseconds::zero()) {
    return utils::Status::InvalidArgument("owner lease TTL must be positive");
  }
  try {
    // PEXPIRE XX never creates a missing key or an expiry-less string.
    if (redis_->command<long long>("PEXPIRE", std::string(key), ttl.count(), "XX") != 1) {
      return utils::Status::Unavailable("owner lease expired or was revoked");
    }
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("owner PEXPIRE XX", error);
  } catch (const sw::redis::Error &error) {
    return utils::Status::OutcomeUnknown("Redis owner PEXPIRE XX outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::RevokeOwnerLease(std::string_view key) {
  utils::ExpectInThreadDomain();
  try {
    (void)redis_->del(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("owner DEL", error);
  } catch (const sw::redis::Error &error) {
    return utils::Status::OutcomeUnknown("Redis owner DEL outcome is ambiguous: " + std::string(error.what()));
  }
}

utils::Status RedisMetaClient::Transact(const std::function<utils::Status(RedisKvTxn &)> &callback) {
  utils::ExpectInThreadDomain();
  for (int attempt = 0; attempt < retry_attempts_; ++attempt) {
    bool retry = false;
    try {
      RedisKvTxn transaction(*redis_);
      try {
        auto status = callback(transaction);
        if (!status.ok()) {
          transaction.Discard();
          return status;
        }

        status = transaction.Commit();
        return status;
      } catch (const RedisKvTxn::WatchConflict &) {
        retry = true;
      } catch (const sw::redis::ReplyError &error) {
        return RedisError("transaction", error);
      } catch (const sw::redis::Error &error) {
        // Only exceptions raised by RedisKvTxn at a known pre-EXEC boundary
        // are retry control flow. Exceptions escaping callback code remain
        // ordinary failures and are never promoted into a blind replay.
        if (!transaction.retryable_pre_exec_failure_) {
          return RedisError("transaction", error);
        }
        retry = true;
      }
    } catch (const sw::redis::ReplyError &error) {
      // Construction may acquire/connect a pooled connection but cannot have
      // executed metadata mutations. A valid Redis reply is still a known
      // terminal failure rather than a transport retry condition.
      return RedisError("transaction", error);
    } catch (const sw::redis::Error &error) {
      // RedisKvTxn construction occurs before MULTI/EXEC, so opaque transport
      // or protocol failures here are safe to retry within the same budget.
      (void)error;
      retry = true;
    }

    if (retry && attempt + 1 < retry_attempts_) {
      Backoff(attempt, retry_backoff_);
    }
  }

  return utils::Status::Unavailable("Redis transaction retry limit exceeded");
}

}  // namespace swordfs::metadata
