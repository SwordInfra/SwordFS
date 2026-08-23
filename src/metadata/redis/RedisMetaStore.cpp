// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaStore.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

#include <folly/Random.h>

namespace swordfs::metadata {
namespace {

sw::redis::ConnectionOptions MakeConnectionOptions(const RedisMetaConfig& config) {
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  options.db = config.db;
  options.connect_timeout = std::chrono::seconds(2);
  options.socket_timeout = std::chrono::seconds(5);
  if (config.username.has_value()) {
    options.user = *config.username;
  }
  if (config.password.has_value()) {
    options.password = *config.password;
  }
  return options;
}

void Backoff(int attempt) {
  constexpr int kBaseDelayMs = 1;
  constexpr int kMaxDelayMs = 32;
  const int exponent = std::min(attempt, 5);
  const int max_delay = std::min(kBaseDelayMs << exponent, kMaxDelayMs);
  const auto delay = folly::Random::rand32(max_delay + 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

utils::Status RedisError(const char* operation, const sw::redis::Error& error) {
  return utils::Status::IOError("Redis " + std::string(operation) +
                                " failed: " + error.what());
}

}  // namespace

RedisMetaTxn::RedisMetaTxn(sw::redis::Redis& redis)
    : transaction_(redis.transaction()) {}

utils::Status RedisMetaTxn::Watch(std::string_view key) {
  try {
    transaction_->redis().watch(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError&) {
    throw;
  } catch (const sw::redis::ClosedError&) {
    throw;
  } catch (const sw::redis::Error& error) {
    return RedisError("WATCH", error);
  }
}

utils::Status RedisMetaTxn::Get(std::string_view key,
                                std::optional<std::string>* value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  try {
    *value = transaction_->redis().get(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError&) {
    throw;
  } catch (const sw::redis::ClosedError&) {
    throw;
  } catch (const sw::redis::Error& error) {
    return RedisError("GET", error);
  }
}

utils::Status RedisMetaTxn::Set(std::string_view key, std::string_view value) {
  try {
    transaction_->set(std::string(key), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError&) {
    throw;
  } catch (const sw::redis::ClosedError&) {
    throw;
  } catch (const sw::redis::Error& error) {
    return RedisError("SET", error);
  }
}

void RedisMetaTxn::Discard() noexcept {
  if (!has_writes_) {
    return;
  }
  try {
    transaction_->discard();
  } catch (const sw::redis::Error&) {
  }
}

utils::Status RedisMetaTxn::Commit() {
  if (!has_writes_) {
    return utils::Status::OK();
  }
  try {
    transaction_->exec();
    return utils::Status::OK();
  } catch (const sw::redis::WatchError&) {
    return utils::Status::Busy("Redis watched key changed");
  } catch (const sw::redis::TimeoutError& error) {
    // TODO(#115): expose an explicit ambiguous-commit status so callers do
    // not need to classify this condition from the error message.
    return utils::Status::IOError(
        "Redis transaction commit is ambiguous after EXEC: " +
        std::string(error.what()));
  } catch (const sw::redis::ClosedError& error) {
    return utils::Status::IOError(
        "Redis transaction commit is ambiguous after EXEC: " +
        std::string(error.what()));
  } catch (const sw::redis::Error& error) {
    return RedisError("transaction EXEC", error);
  }
}

RedisMetaStore::RedisMetaStore(const RedisMetaConfig& config)
    : redis_(std::make_unique<sw::redis::Redis>(MakeConnectionOptions(config))) {}

utils::Status RedisMetaStore::Ping() {
  try {
    redis_->ping();
    return utils::Status::OK();
  } catch (const sw::redis::Error& error) {
    return RedisError("PING", error);
  }
}

utils::Status RedisMetaStore::Transact(
    const std::function<utils::Status(RedisMetaTxn&)>& callback,
    int max_attempts) {
  if (max_attempts <= 0) {
    return utils::Status::InvalidArgument("max_attempts must be positive");
  }

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    try {
      RedisMetaTxn transaction(*redis_);
      auto status = callback(transaction);
      if (!status.ok()) {
        transaction.Discard();
        if (status.IsBusy()) {
          Backoff(attempt);
          continue;
        }
        return status;
      }

      status = transaction.Commit();
      if (status.IsBusy()) {
        Backoff(attempt);
        continue;
      }
      return status;
    } catch (const sw::redis::TimeoutError&) {
      Backoff(attempt);
    } catch (const sw::redis::ClosedError&) {
      Backoff(attempt);
    } catch (const sw::redis::Error& error) {
      return RedisError("transaction", error);
    }
  }

  return utils::Status::Busy("Redis transaction retry limit exceeded");
}

}  // namespace swordfs::metadata
