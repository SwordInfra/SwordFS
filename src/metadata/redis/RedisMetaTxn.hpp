// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaClient;

// One optimistic metadata transaction attempt backed by Redis. All reads,
// queued writes and EXEC operations use the same connection checked out from
// the Redis client's connection pool. Redis WATCH/MULTI/EXEC details are kept
// inside this implementation; callers only see key/value operations.
// The object is only valid for the lifetime of RedisMetaClient::Transact().
class RedisMetaTxn {
 public:
  // WATCH is issued before each read so Redis can detect changes between the
  // read and EXEC. Redis WATCH/MULTI details remain private to this class.
  utils::Status Get(std::string_view key, std::string *value);
  utils::Status HGet(std::string_view key, std::string_view field, std::string *value);
  utils::Status HScan(std::string_view key, uint64_t cursor, size_t count,
                      std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor);
  utils::Status HLen(std::string_view key, uint64_t *length);
  utils::Status Set(std::string_view key, std::string_view value);
  utils::Status HSet(std::string_view key, std::string_view field, std::string_view value);
  utils::Status HDel(std::string_view key, std::string_view field);
  utils::Status IncrBy(std::string_view key, int64_t delta);
  utils::Status Del(std::string_view key);

 private:
  friend class RedisMetaClient;
  explicit RedisMetaTxn(sw::redis::Redis &redis);

  void Discard() noexcept;
  utils::Status Commit();

  // Finish a read-only transaction with a harmless queued command so
  // redis-plus-plus returns the checked-out pooled connection instead of
  // invalidating it when QueuedRedis is destroyed.
  utils::Status ReleaseConnection();

 private:
  std::optional<sw::redis::Transaction> transaction_;
  // Keep the Redis view returned by Transaction::redis() alive for the
  // entire transaction. redis-plus-plus requires this for pooled
  // transaction connections.
  std::optional<sw::redis::Redis> redis_;
  bool has_writes_ = false;
};

}  // namespace swordfs::metadata
