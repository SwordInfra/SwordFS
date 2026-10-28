// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sw/redis++/redis++.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Synchronous Redis adapter. This component is intentionally thread-domain
// only: callers must perform any fiber -> thread transition before invoking it.
class RedisMetaClient {
 public:
  explicit RedisMetaClient(const RedisMetaConfig &config);
  ~RedisMetaClient();

  RedisMetaClient(const RedisMetaClient &) = delete;
  RedisMetaClient &operator=(const RedisMetaClient &) = delete;

  utils::Status Ping();
  utils::Status Get(std::string_view key, std::string *value);
  utils::Status MGet(const std::vector<std::string> &keys, std::vector<std::optional<std::string>> *values);
  // #515: server-atomic, read-only attachment snapshot. A caller reads and
  // parses the inode first to derive the EOF boundary, then this EVAL_RO
  // validates that exact inode image while reading both attachment fields.
  // "matched = false" means retry from a fresh inode, never return stale data.
  utils::Status ReadChunkAttachmentsIfUnchanged(std::string_view inode_key, std::string_view refs_key,
                                                std::string_view expected_inode, uint64_t index, uint64_t eof_index,
                                                std::optional<std::string> *attachment,
                                                std::optional<std::string> *eof_attachment, bool *matched);
  utils::Status Incr(std::string_view key, uint64_t *value);
  // Atomically increment a missing/non-negative integer counter. Negative,
  // malformed, and overflowed state is a known failure and is not mutated.
  // Transport/protocol loss after the script may have executed is
  // OutcomeUnknown, matching standalone INCR allocation semantics.
  utils::Status IncrNonNegative(std::string_view key, uint64_t *value);
  // WATCH/GET/MULTI/SET/EXEC; missing/corrupt counters must not restart an
  // existing volume-scoped identity stream. No Lua or blind replay.
  utils::Status IncrExistingNonNegative(std::string_view key, uint64_t *value);
  // Native ordered LIST records; no header, empty marker, or Lua script.
  // Creation requires a first record; subsequent appends require an existing
  // LIST and never recreate a missing authoritative index.
  utils::Status LRange(std::string_view key, std::vector<std::string> *values);
  utils::Status CreateListWithFirstRecord(std::string_view key, std::string_view value);
  utils::Status RPushExisting(std::string_view key, std::string_view value);
  // Native Redis 7 owner-lease operations. No retry after an ambiguous
  // mutation: a lost SET/PEXPIRE/DEL reply is not a confirmed new lease.
  utils::Status CreateOwnerLease(std::string_view key, std::string_view nonce, std::chrono::milliseconds ttl);
  utils::Status RenewOwnerLease(std::string_view key, std::chrono::milliseconds ttl);
  utils::Status RevokeOwnerLease(std::string_view key);
  utils::Status HScan(std::string_view key, uint64_t cursor, size_t count,
                      std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor);

  // Runs one optimistic transaction entirely on the calling POSIX thread.
  // WATCH, reads, MULTI, queued writes and EXEC share one Redis connection.
  // Only Redis WATCH conflicts and pre-commit connection failures are
  // retried; callback statuses, including filesystem Busy, are returned.
  // Exhausting that safe retry policy returns Unavailable. Post-EXEC
  // ambiguity is classified by RedisKvTxn as OutcomeUnknown and is never
  // automatically replayed here.
  utils::Status Transact(const std::function<utils::Status(RedisKvTxn &)> &callback);

 private:
  std::unique_ptr<sw::redis::Redis> redis_;
  int retry_attempts_;
  std::chrono::milliseconds retry_backoff_;
};

}  // namespace swordfs::metadata
