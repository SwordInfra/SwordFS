// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaClient.hpp"

#include <folly/Random.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#include "config/ConfigCenter.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "utils/FiberThreadPool.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {
namespace {

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
  constexpr int kMaxDelayMs = 1000;
  const int exponent = std::min(attempt, 10);
  const auto max_delay = std::min(base_delay * (1 << exponent), std::chrono::milliseconds(kMaxDelayMs));
  const auto delay = std::chrono::milliseconds(folly::Random::rand64(max_delay.count() + 1));
  std::this_thread::sleep_for(delay);
}

utils::Status RedisError(const char *operation, const sw::redis::Error &error) {
  return utils::Status::IOError("Redis " + std::string(operation) + " failed: " + error.what());
}

}  // namespace

RedisMetaClient::RedisMetaClient(const RedisMetaConfig &config)
    : pool_(std::make_unique<utils::FiberThreadPool>(
          static_cast<size_t>(swordfs::config::ConfigCenter::Instance().meta_thread_count()))),
      retry_attempts_(config.retry_attempts),
      retry_backoff_(config.retry_backoff) {
  if (retry_attempts_ <= 0) {
    throw std::invalid_argument("retry_attempts must be positive");
  }
  sw::redis::ConnectionPoolOptions pool_options;
  pool_options.size = config.pool_size;
  pool_options.wait_timeout = config.pool_wait_timeout;
  redis_ = std::make_unique<sw::redis::Redis>(MakeConnectionOptions(config), pool_options);
}

RedisMetaClient::~RedisMetaClient() = default;

utils::Status RedisMetaClient::Ping() {
  return pool_->Run([this] {
    try {
      redis_->ping();
      return utils::Status::OK();
    } catch (const sw::redis::Error &error) {
      return RedisError("PING", error);
    }
  });
}

utils::Status RedisMetaClient::Get(std::string_view key, std::string *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  return pool_->Run([this, key, value] {
    try {
      auto result = redis_->get(std::string(key));
      if (!result.has_value()) {
        return utils::Status::NotFound("Redis key not found");
      }
      *value = std::move(*result);
      return utils::Status::OK();
    } catch (const sw::redis::Error &error) {
      return RedisError("GET", error);
    }
  });
}

utils::Status RedisMetaClient::TransactImpl(const std::function<utils::Status(RedisMetaTxn &)> &callback) {
  for (int attempt = 0; attempt < retry_attempts_; ++attempt) {
    try {
      RedisMetaTxn transaction(*redis_);
      auto status = callback(transaction);
      if (!status.ok()) {
        transaction.Discard();
        if (status.IsBusy()) {
          Backoff(attempt, retry_backoff_);
          continue;
        }
        return status;
      }

      status = transaction.Commit();
      if (status.IsBusy()) {
        Backoff(attempt, retry_backoff_);
        continue;
      }
      return status;
    } catch (const sw::redis::TimeoutError &) {
      Backoff(attempt, retry_backoff_);
    } catch (const sw::redis::ClosedError &) {
      Backoff(attempt, retry_backoff_);
    } catch (const sw::redis::Error &error) {
      return RedisError("transaction", error);
    }
  }

  return utils::Status::Busy("Redis transaction retry limit exceeded");
}

utils::Status RedisMetaClient::Transact(const std::function<utils::Status(RedisMetaTxn &)> &callback) {
  return pool_->Run([this, &callback] { return TransactImpl(callback); });
}

}  // namespace swordfs::metadata
