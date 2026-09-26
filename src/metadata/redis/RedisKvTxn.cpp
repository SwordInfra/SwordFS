// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisKvTxn.hpp"

#include <folly/logging/xlog.h>

#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {
namespace {

utils::Status RedisError(const char *operation, const sw::redis::Error &error) {
  return utils::Status::IOError("Redis " + std::string(operation) + " failed: " + error.what());
}

utils::Status RedisUnavailable(const char *operation, const sw::redis::Error &error) {
  return utils::Status::Unavailable("Redis " + std::string(operation) + " unavailable: " + error.what());
}

utils::Status ValidateWriteExecReplies(sw::redis::QueuedReplies replies) {
  for (std::size_t i = 0; i < replies.size(); ++i) {
    try {
      (void)replies.get(i);
    } catch (const sw::redis::Error &error) {
      // Redis MULTI/EXEC does not roll back commands that succeeded before a
      // later command returned an error. Surface this explicitly so callers
      // can reconcile authoritative state instead of treating a potentially
      // partial commit as either success or rollback.
      return utils::Status::OutcomeUnknown("Redis transaction EXEC command failed; commit may be partial: " +
                                           std::string(error.what()));
    }
  }
  return utils::Status::OK();
}

sw::redis::Transaction CreateTransaction(sw::redis::Redis &redis) {
  utils::ExpectInThreadDomain();
  return redis.transaction(false, false);
}

template <typename Fn>
utils::Status RunRedisCommand(const char *operation, bool *retryable_pre_exec_failure, Fn &&fn) {
  utils::ExpectInThreadDomain();
  try {
    return fn();
  } catch (const sw::redis::ReplyError &error) {
    // A valid Redis error reply is a known terminal command result. It is not
    // a transport failure and must not be blindly replayed.
    return RedisError(operation, error);
  } catch (const sw::redis::Error &error) {
    // MULTI queues writes; none of them can execute until EXEC. Therefore an
    // opaque transport/protocol failure while WATCHing, reading, or queuing a
    // command is safe for RedisMetaClient to retry from a fresh transaction.
    *retryable_pre_exec_failure = true;
    throw;
  }
}

}  // namespace

RedisKvTxn::RedisKvTxn(sw::redis::Redis &redis)
    : transaction_(std::make_unique<sw::redis::Transaction>(CreateTransaction(redis))),
      redis_(std::make_unique<sw::redis::Redis>(transaction_->redis())) {
}

utils::Status RedisKvTxn::Get(std::string_view key, std::string *value) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis GET output is null");
  }
  if (has_writes_) {
    return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
  }
  return RunRedisCommand("GET", &retryable_pre_exec_failure_, [&] {
    redis_->watch(key);
    auto result = redis_->get(key);
    if (!result.has_value()) {
      return utils::Status::NotFound("Redis key not found");
    }
    *value = std::move(*result);
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HGet(std::string_view key, std::string_view field, std::string *value, ReadMode mode) {
  if (value == nullptr) {
    return utils::Status::InvalidArgument("Redis HGET output is null");
  }
  if (has_writes_) {
    return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
  }
  return RunRedisCommand("HGET", &retryable_pre_exec_failure_, [&] {
    if (mode == ReadMode::kWatched) {
      redis_->watch(key);
    }
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
  return RunRedisCommand("HLEN", &retryable_pre_exec_failure_, [&] {
    redis_->watch(key);
    *length = redis_->hlen(std::string(key));
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HScan(std::string_view key, uint64_t cursor, size_t count,
                                std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor) {
  if (values == nullptr || next_cursor == nullptr) {
    return utils::Status::InvalidArgument("Redis HSCAN output is null");
  }
  if (has_writes_) {
    return utils::Status::InvalidArgument("Redis transaction cannot read after a write");
  }
  return RunRedisCommand("HSCAN", &retryable_pre_exec_failure_, [&] {
    redis_->watch(key);
    values->clear();
    *next_cursor = cursor;
    *next_cursor =
        redis_->hscan(std::string(key), *next_cursor, static_cast<long long>(count), std::back_inserter(*values));
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::Set(std::string_view key, std::string_view value) {
  return RunRedisCommand("SET", &retryable_pre_exec_failure_, [&] {
    transaction_->set(std::string(key), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HSet(std::string_view key, std::string_view field, std::string_view value) {
  return RunRedisCommand("HSET", &retryable_pre_exec_failure_, [&] {
    transaction_->hset(std::string(key), std::string(field), std::string(value));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::HDel(std::string_view key, std::string_view field) {
  return RunRedisCommand("HDEL", &retryable_pre_exec_failure_, [&] {
    transaction_->hdel(std::string(key), std::string(field));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::IncrBy(std::string_view key, int64_t delta) {
  return RunRedisCommand("INCRBY", &retryable_pre_exec_failure_, [&] {
    transaction_->incrby(std::string(key), delta);
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::Del(std::string_view key) {
  return RunRedisCommand("DEL", &retryable_pre_exec_failure_, [&] {
    transaction_->del(std::string(key));
    has_writes_ = true;
    return utils::Status::OK();
  });
}

utils::Status RedisKvTxn::ReleaseConnection() {
  utils::ExpectInThreadDomain();
  try {
    transaction_->ping();
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("read-only transaction PING", error);
  } catch (const sw::redis::Error &) {
    // PING is only queued here. No EXEC has been issued, so a transport or
    // protocol failure remains safe for RedisMetaClient's bounded retry.
    retryable_pre_exec_failure_ = true;
    throw;
  }

  try {
    transaction_->exec();
    return utils::Status::OK();
  } catch (const sw::redis::WatchError &) {
    throw WatchConflict{};
  } catch (const sw::redis::ReplyError &error) {
    return RedisError("read-only transaction EXEC", error);
  } catch (const sw::redis::Error &error) {
    // No mutation can become ambiguous in a read-only transaction. Once EXEC
    // is issued, failure to obtain a reliable reply is terminal unavailability.
    return RedisUnavailable("read-only transaction EXEC", error);
  }
}

void RedisKvTxn::ReleaseRedisView() {
  // transaction(false, false) borrows a connection from the shared redis++
  // pool. The Redis view returned by Transaction::redis() shares that guarded
  // connection, so it must be released before a normal terminal operation.
  // Otherwise redis++ cannot return the healthy connection during _reset(),
  // and the later QueuedRedis destructor invalidates it before returning it to
  // the pool.
  redis_.reset();
}

void RedisKvTxn::Discard() noexcept {
  utils::ExpectInThreadDomain();
  ReleaseRedisView();
  try {
    if (!has_writes_) {
      transaction_->ping();
    }
    transaction_->discard();
  } catch (const sw::redis::Error &error) {
    SWORDFS_LOG_DEBUG << "Redis transaction discard failed: " << error.what();
  }
}

utils::Status RedisKvTxn::Commit() {
  utils::ExpectInThreadDomain();
  ReleaseRedisView();
  if (!has_writes_) {
    return ReleaseConnection();
  }
  try {
    return ValidateWriteExecReplies(transaction_->exec());
  } catch (const sw::redis::WatchError &) {
    (void)ReleaseConnection();
    throw WatchConflict{};
  } catch (const sw::redis::ReplyError &error) {
    // A top-level Redis error reply (for example EXECABORT) is a reliable
    // server result, unlike an error while receiving/decoding the EXEC reply.
    return RedisError("transaction EXEC", error);
  } catch (const sw::redis::Error &error) {
    // redis++'s exec() call contains both sending EXEC and receiving/parsing
    // its reply. Once this boundary is entered, an opaque I/O/protocol error
    // cannot prove whether Redis executed the queued mutations.
    SWORDFS_LOG_WARN << "Redis transaction EXEC result is ambiguous: " << error.what();
    return utils::Status::OutcomeUnknown("Redis transaction commit is ambiguous after EXEC: " +
                                         std::string(error.what()));
  }
}

}  // namespace swordfs::metadata
