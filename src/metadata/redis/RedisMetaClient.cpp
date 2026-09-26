// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaClient.hpp"

#include <folly/Random.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#include "metadata/redis/RedisKvTxn.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {
namespace {

constexpr auto kMaxRetryBackoff = std::chrono::milliseconds(1000);

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

utils::Status RedisMetaClient::HGet(std::string_view key, std::string_view field, std::string *value) {
  utils::ExpectInThreadDomain();
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis HGET output is null");
  }
  return RunReadOnlyCommand("HGET", [&] {
    auto result = redis_->hget(std::string(key), std::string(field));
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis hash field not found");
    }
    *value = std::move(*result);
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
