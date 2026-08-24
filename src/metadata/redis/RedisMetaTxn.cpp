// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaTxn.hpp"

#include "metadata/redis/RedisMetaClient.hpp"

namespace swordfs::metadata {
namespace {

utils::Status RedisError(const char *operation, const sw::redis::Error &error) {
  return utils::Status::IOError("Redis " + std::string(operation) + " failed: " + error.what());
}

}  // namespace

RedisMetaTxn::RedisMetaTxn(sw::redis::Redis &redis) : transaction_(redis.transaction(false, false)) {
}

utils::Status RedisMetaTxn::Watch(std::string_view key) {
  try {
    transaction_->redis().watch(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("WATCH", error);
  }
}

utils::Status RedisMetaTxn::Get(std::string_view key, std::optional<std::string> *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  try {
    *value = transaction_->redis().get(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("GET", error);
  }
}

utils::Status RedisMetaTxn::Set(std::string_view key, std::string_view value) {
  try {
    transaction_->set(std::string(key), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("SET", error);
  }
}

void RedisMetaTxn::Discard() noexcept {
  if (!has_writes_) {
    return;
  }
  try {
    transaction_->discard();
  } catch (const sw::redis::Error &) {
  }
}

utils::Status RedisMetaTxn::Commit() {
  if (!has_writes_) {
    return utils::Status::OK();
  }
  try {
    transaction_->exec();
    return utils::Status::OK();
  } catch (const sw::redis::WatchError &) {
    return utils::Status::Busy("Redis watched key changed");
  } catch (const sw::redis::TimeoutError &error) {
    // TODO(#115): expose an explicit ambiguous-commit status so callers do
    // not need to classify this condition from the error message.
    return utils::Status::IOError("Redis transaction commit is ambiguous after EXEC: " + std::string(error.what()));
  } catch (const sw::redis::ClosedError &error) {
    return utils::Status::IOError("Redis transaction commit is ambiguous after EXEC: " + std::string(error.what()));
  } catch (const sw::redis::Error &error) {
    return RedisError("transaction EXEC", error);
  }
}

}  // namespace swordfs::metadata
