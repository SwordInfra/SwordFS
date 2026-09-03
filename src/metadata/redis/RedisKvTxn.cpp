// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisKvTxn.hpp"

#include <folly/logging/xlog.h>

#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {
namespace {

utils::Status RedisError(const char *operation, const sw::redis::Error &error) {
  return utils::Status::IOError("Redis " + std::string(operation) + " failed: " + error.what());
}

template <typename Fn>
utils::Status RunRedisCommand(const char *operation, Fn &&fn) {
  try {
    return fn();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError(operation, error);
  }
}

}  // namespace

RedisKvTxn::RedisKvTxn(sw::redis::Redis &redis)
    : transaction_(redis.transaction(false, false)), redis_(transaction_->redis()) {
}

utils::Status RedisKvTxn::Get(std::string_view key, std::string *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  if (has_writes_) {
    return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
  }
  return RunRedisCommand("GET", [&] {
    redis_->watch(key);
    auto result = redis_->get(key);
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis key not found");
    }
    *value = std::move(*result);
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HGet(std::string_view key, std::string_view field, std::string *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis HGET output is null");
  }
  if (has_writes_) {
    return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
  }
  return RunRedisCommand("HGET", [&] {
    redis_->watch(key);
    auto result = redis_->hget(std::string(key), std::string(field));
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis hash field not found");
    }
    *value = std::move(*result);
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HLen(std::string_view key, uint64_t *length) {
  if (length == nullptr) {
    return utils::Status::InvalidArgument("Redis HLEN output is null");
  }
  if (has_writes_) {
    return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
  }
  return RunRedisCommand("HLEN", [&] {
    redis_->watch(key);
    *length = redis_->hlen(std::string(key));
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::Set(std::string_view key, std::string_view value) {
  return RunRedisCommand("SET", [&] {
    transaction_->set(std::string(key), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HSet(std::string_view key, std::string_view field, std::string_view value) {
  return RunRedisCommand("HSET", [&] {
    transaction_->hset(std::string(key), std::string(field), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HDel(std::string_view key, std::string_view field) {
  return RunRedisCommand("HDEL", [&] {
    transaction_->hdel(std::string(key), std::string(field));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::IncrBy(std::string_view key, int64_t delta) {
  return RunRedisCommand("INCRBY", [&] {
    transaction_->incrby(std::string(key), delta);
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::Del(std::string_view key) {
  return RunRedisCommand("DEL", [&] {
    transaction_->del(std::string(key));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::ReleaseConnection() {
  try {
    transaction_->ping();
    transaction_->exec();
    return utils::Status::OK();
  } catch (const sw::redis::WatchError &) {
    return utils::Status::Busy("Redis watched key changed");
  } catch (const sw::redis::TimeoutError &error) {
    return utils::Status::IOError("Redis read-only transaction timed out: " + std::string(error.what()));
  } catch (const sw::redis::ClosedError &error) {
    return utils::Status::IOError("Redis read-only transaction connection closed: " + std::string(error.what()));
  } catch (const sw::redis::Error &error) {
    return RedisError("read-only transaction", error);
  }
}

void RedisKvTxn::Discard() noexcept {
  try {
    if (!has_writes_) {
      transaction_->ping();
    }
    transaction_->discard();
  } catch (const sw::redis::Error &) {
  }
}

utils::Status RedisKvTxn::Commit() {
  if (!has_writes_) {
    return ReleaseConnection();
  }
  try {
    transaction_->exec();
    return utils::Status::OK();
  } catch (const sw::redis::WatchError &) {
    (void)ReleaseConnection();
    return utils::Status::Busy("Redis watched key changed");
  } catch (const sw::redis::TimeoutError &error) {
    SWORDFS_LOG_WARN << "Redis transaction EXEC timed out; commit result is ambiguous: " << error.what();
    return utils::Status::IOError("Redis transaction commit is ambiguous after EXEC: " + std::string(error.what()));
  } catch (const sw::redis::ClosedError &error) {
    SWORDFS_LOG_WARN << "Redis transaction EXEC connection closed; commit result is ambiguous: " << error.what();
    return utils::Status::IOError("Redis transaction commit is ambiguous after EXEC: " + std::string(error.what()));
  } catch (const sw::redis::Error &error) {
    return RedisError("transaction EXEC", error);
  }
}

}  // namespace swordfs::metadata
