// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <sw/redis++/redis++.h>

#include "metadata/redis/RedisMetaConfig.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// One optimistic Redis transaction attempt. All WATCH, reads, MULTI, queued
// writes and EXEC operations use the same dedicated Redis client/connection.
// Call Watch() and all reads before the first Set(): the first queued write
// opens MULTI, after which Redis no longer returns ordinary read replies and
// rejects WATCH. The object is only valid for the lifetime of
// RedisMetaStore::Transact().
class RedisMetaTxn {
 public:
  utils::Status Watch(std::string_view key);
  utils::Status Get(std::string_view key, std::optional<std::string>* value);
  utils::Status Set(std::string_view key, std::string_view value);

 private:
  friend class RedisMetaStore;
  explicit RedisMetaTxn(std::unique_ptr<sw::redis::Redis> redis);

  void Discard() noexcept;
  utils::Status Commit();

  std::unique_ptr<sw::redis::Redis> redis_;
  std::optional<sw::redis::Transaction> transaction_;
  bool has_writes_ = false;
};

class RedisMetaStore {
 public:
  static constexpr int kDefaultMaxAttempts = 8;

  explicit RedisMetaStore(const RedisMetaConfig& config);

  RedisMetaStore(const RedisMetaStore&) = delete;
  RedisMetaStore& operator=(const RedisMetaStore&) = delete;

  utils::Status Ping();

  // Runs an optimistic transaction. Every attempt owns one dedicated Redis
  // connection so WATCH, reads, MULTI, queued writes and EXEC share the same
  // connection. Busy and WATCH conflicts retry with bounded backoff.
  //
  // Known Phase 0 limitation: attempts create a fresh Redis client/connection
  // instead of reusing a connection pool. This favors simple connection
  // ownership and correctness; connection reuse should be revisited in the
  // Phase 6 performance work.
  utils::Status Transact(const std::function<utils::Status(RedisMetaTxn&)>& callback,
                         int max_attempts = kDefaultMaxAttempts);

 private:
  RedisMetaConfig config_;
  std::unique_ptr<sw::redis::Redis> redis_;
};

}  // namespace swordfs::metadata
