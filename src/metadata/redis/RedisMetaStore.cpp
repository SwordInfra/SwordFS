// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaStore.hpp"

#include <exception>
#include <string>

namespace swordfs::metadata {
namespace {

sw::redis::ConnectionOptions MakeConnectionOptions(const RedisMetaConfig& config) {
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  options.db = config.db;
  if (config.username.has_value()) {
    options.user = *config.username;
  }
  if (config.password.has_value()) {
    options.password = *config.password;
  }
  return options;
}

}  // namespace

RedisMetaStore::RedisMetaStore(const RedisMetaConfig& config)
    : redis_(std::make_unique<sw::redis::Redis>(MakeConnectionOptions(config))) {}

utils::Status RedisMetaStore::Ping() {
  try {
    redis_->ping();
    return utils::Status::OK();
  } catch (const sw::redis::Error& error) {
    return utils::Status::IOError("Redis PING failed: " + std::string(error.what()));
  }
}

utils::Status RedisMetaStore::Transact(
    const std::function<utils::Status(sw::redis::Transaction&)>& callback,
    int max_attempts) {
  if (max_attempts <= 0) {
    return utils::Status::InvalidArgument("max_attempts must be positive");
  }

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    try {
      auto transaction = redis_->transaction();
      auto status = callback(transaction);
      if (status.IsBusy()) {
        continue;
      }
      if (!status.ok()) {
        transaction.discard();
        return status;
      }

      try {
        transaction.exec();
        return utils::Status::OK();
      } catch (const sw::redis::WatchError&) {
        continue;
      } catch (const sw::redis::TimeoutError& error) {
        return utils::Status::IOError(
            "Redis transaction commit is ambiguous after EXEC: " +
            std::string(error.what()));
      } catch (const sw::redis::ClosedError& error) {
        return utils::Status::IOError(
            "Redis transaction commit is ambiguous after EXEC: " +
            std::string(error.what()));
      } catch (const sw::redis::Error& error) {
        return utils::Status::IOError("Redis transaction EXEC failed: " +
                                      std::string(error.what()));
      }
    } catch (const sw::redis::TimeoutError&) {
      continue;
    } catch (const sw::redis::ClosedError&) {
      continue;
    } catch (const sw::redis::Error& error) {
      return utils::Status::IOError("Redis transaction failed: " +
                                    std::string(error.what()));
    }
  }

  return utils::Status::Busy("Redis transaction retry limit exceeded");
}

}  // namespace swordfs::metadata
