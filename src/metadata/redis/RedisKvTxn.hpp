// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <optional>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaClient;

// One optimistic Redis key/value transaction attempt. All reads, queued
// writes and EXEC operations use the same connection checked out from the
// Redis client's connection pool. This layer deliberately knows nothing
// about SwordFS metadata schemas or POSIX semantics.
class RedisKvTxn {
 public:
  utils::Status Get(std::string_view key, std::string *value);
  utils::Status HGet(std::string_view key, std::string_view field, std::string *value);
  utils::Status HLen(std::string_view key, uint64_t *length);
  utils::Status Set(std::string_view key, std::string_view value);
  utils::Status HSet(std::string_view key, std::string_view field, std::string_view value);
  utils::Status HDel(std::string_view key, std::string_view field);
  utils::Status IncrBy(std::string_view key, int64_t delta);
  utils::Status Del(std::string_view key);

 private:
  friend class RedisMetaClient;
  explicit RedisKvTxn(sw::redis::Redis &redis);

  void Discard() noexcept;
  utils::Status Commit();
  utils::Status ReleaseConnection();

 private:
  std::optional<sw::redis::Transaction> transaction_;
  std::optional<sw::redis::Redis> redis_;
  bool has_writes_ = false;
};

}  // namespace swordfs::metadata
