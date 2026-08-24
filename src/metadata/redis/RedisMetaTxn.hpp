// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <optional>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisMetaStore;

// One optimistic Redis transaction attempt. All WATCH, reads, MULTI, queued
// writes and EXEC operations use the same dedicated Redis connection.
// Call Watch() and all reads before the first Set(): the first queued write
// opens MULTI, after which Redis no longer returns ordinary read replies and
// rejects WATCH. The object is only valid for the lifetime of
// RedisMetaStore::Transact().
class RedisMetaTxn {
public:
  utils::Status Watch(std::string_view key);
  utils::Status Get(std::string_view key, std::optional<std::string> *value);
  utils::Status Set(std::string_view key, std::string_view value);

private:
  friend class RedisMetaStore;
  explicit RedisMetaTxn(sw::redis::Redis &redis);

  void Discard() noexcept;
  utils::Status Commit();

  std::optional<sw::redis::Transaction> transaction_;
  bool has_writes_ = false;
};

}  // namespace swordfs::metadata
