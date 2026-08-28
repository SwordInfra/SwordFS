// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <string>
#include <string_view>

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
  utils::Status Set(std::string_view key, std::string_view value);
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

  std::optional<sw::redis::Transaction> transaction_;
  bool has_writes_ = false;
};

}  // namespace swordfs::metadata
