// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaTxn.hpp"

#include <folly/logging/xlog.h>

#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {
namespace {

utils::Status RedisError(const char *operation, const sw::redis::Error &error) {
  return utils::Status::IOError("Redis " + std::string(operation) + " failed: " + error.what());
}

}  // namespace

RedisMetaTxn::RedisMetaTxn(sw::redis::Redis &redis)
    // Use a pooled connection for the whole transaction. WATCH and the
    // subsequent read must execute on the same connection as EXEC.
    : transaction_(redis.transaction(false, false)), redis_(transaction_->redis()) {
}

utils::Status RedisMetaTxn::Get(std::string_view key, std::string *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  try {
    // GET is executed immediately because callers need its value to compute
    // subsequent metadata mutations. Once a write is queued, redis-plus-plus
    // has entered MULTI, so a subsequent GET through the transaction would be
    // queued instead of returning its value. Keep this transaction model's
    // read-before-write restriction rather than mixing immediate reads with
    // queued commands.
    if (has_writes_) {
      return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
    }
    // WATCH must happen before the read so changes between GET and EXEC are
    // detected. This is Redis's optimistic transaction pattern: the read is
    // performed immediately, while queued writes are committed by EXEC.
    redis_->watch(key);
    auto result = redis_->get(key);
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis key not found");
    }
    *value = std::move(*result);
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("GET", error);
  }
}

utils::Status RedisMetaTxn::HGet(std::string_view key, std::string_view field, std::string *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis HGET output is null");
  }
  try {
    if (has_writes_) {
      return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
    }
    redis_->watch(key);
    auto result = redis_->hget(std::string(key), std::string(field));
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis hash field not found");
    }
    *value = std::move(*result);
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("HGET", error);
  }
}

utils::Status RedisMetaTxn::HScan(std::string_view key, uint64_t cursor, size_t count,
                                  std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor) {
  if (values == nullptr || next_cursor == nullptr) {
    return utils::Status::InvalidArgument("Redis HSCAN output is null");
  }
  try {
    if (has_writes_) {
      return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
    }
    redis_->watch(key);
    values->clear();
    *next_cursor = cursor;
    *next_cursor =
        redis_->hscan(std::string(key), *next_cursor, static_cast<long long>(count), std::back_inserter(*values));
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("HSCAN", error);
  }
}

utils::Status RedisMetaTxn::HLen(std::string_view key, uint64_t *length) {
  if (length == nullptr) {
    return utils::Status::InvalidArgument("Redis HLEN output is null");
  }
  try {
    if (has_writes_) {
      return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
    }
    redis_->watch(key);
    *length = redis_->hlen(std::string(key));
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("HLEN", error);
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

utils::Status RedisMetaTxn::HSet(std::string_view key, std::string_view field, std::string_view value) {
  try {
    transaction_->hset(std::string(key), std::string(field), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("HSET", error);
  }
}

utils::Status RedisMetaTxn::HDel(std::string_view key, std::string_view field) {
  try {
    transaction_->hdel(std::string(key), std::string(field));
    has_writes_ = true;
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("HDEL", error);
  }
}

utils::Status RedisMetaTxn::IncrBy(std::string_view key, int64_t delta) {
  try {
    transaction_->incrby(std::string(key), delta);
    has_writes_ = true;
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("INCRBY", error);
  }
}

utils::Status RedisMetaTxn::Del(std::string_view key) {
  try {
    transaction_->del(std::string(key));
    has_writes_ = true;
    return utils::Status::OK();
  } catch (const sw::redis::TimeoutError &) {
    throw;
  } catch (const sw::redis::ClosedError &) {
    throw;
  } catch (const sw::redis::Error &error) {
    return RedisError("DEL", error);
  }
}

utils::Status RedisMetaTxn::ReleaseConnection() {
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

void RedisMetaTxn::Discard() noexcept {
  try {
    if (!has_writes_) {
      // QueuedRedis only returns a pooled connection after EXEC/DISCARD has
      // reset its internal transaction state. Open a harmless transaction so
      // read-only and pre-commit error paths do not invalidate the pool slot.
      transaction_->ping();
    }
    transaction_->discard();
  } catch (const sw::redis::Error &) {
  }
}

utils::Status RedisMetaTxn::Commit() {
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
