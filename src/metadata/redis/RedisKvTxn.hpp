// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaClient;

// One optimistic Redis key/value transaction attempt. All reads, queued
// writes and EXEC operations use the same connection checked out from the
// Redis client's connection pool. This layer deliberately knows nothing
// about SwordFS metadata schemas or POSIX semantics.
class RedisKvTxn {
 public:
  enum class ReadMode {
    kWatched,
    kUnwatched,
  };

  utils::Status Get(std::string_view key, std::string *value);
  // Hash reads are watched by default. kUnwatched is for validation whose
  // serialization is provided by another watched key in the same operation.
  utils::Status HGet(std::string_view key, std::string_view field, std::string *value,
                     ReadMode mode = ReadMode::kWatched);
  utils::Status HLen(std::string_view key, uint64_t *length);
  utils::Status HScan(std::string_view key, uint64_t cursor, size_t count,
                      std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor);
  utils::Status Set(std::string_view key, std::string_view value);
  utils::Status HSet(std::string_view key, std::string_view field, std::string_view value);
  utils::Status HDel(std::string_view key, std::string_view field);
  utils::Status IncrBy(std::string_view key, int64_t delta);
  utils::Status Del(std::string_view key);

 private:
  friend class RedisMetaClient;
  struct WatchConflict {};

  explicit RedisKvTxn(sw::redis::Redis &redis);

  void Discard() noexcept;
  // Returns terminal commit failures as Status. Possible partial/post-EXEC
  // mutation outcomes are OutcomeUnknown. A WATCH conflict remains private
  // Redis control flow and is translated to WatchConflict so RedisMetaClient
  // can retry it without conflating it with Status::Busy or with exceptions
  // escaping from callback code.
  utils::Status Commit();
  utils::Status ReleaseConnection();
  void ReleaseRedisView();

 private:
  std::unique_ptr<sw::redis::Transaction> transaction_;
  std::unique_ptr<sw::redis::Redis> redis_;
  bool has_writes_ = false;
};

}  // namespace swordfs::metadata
