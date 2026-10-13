// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisLockAuthority.hpp"

#include <folly/Format.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metadata/redis/RedisKvTxn.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {
namespace {

constexpr size_t kMaxHolders = 1024;
constexpr size_t kMaxValueBytes = 4096;
constexpr size_t kMaxIntervals = 128;
constexpr int kUncertainAttempts = 3;
constexpr size_t kMaxCachedGcCursors = 256;
constexpr std::string_view kFenceField = "!fence";

using Type = RedisLockAuthority::PosixType;
using Range = RedisLockAuthority::PosixRange;
using Flock = RedisLockAuthority::FlockType;

struct Interval {
  uint64_t start;
  uint64_t end;  // UINT64_MAX is the infinity sentinel.
  Type type;
  int32_t pid = 0;
};

struct Holder {
  uint64_t generation;
  uint64_t kernel_owner;
  std::vector<Interval> ranges;
  Flock flock = Flock::kUnlock;
};

bool ParseNumber(std::string_view text, uint64_t *value) {
  if (text.empty()) {
    return false;
  }
  const auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), *value);
  return error == std::errc{} && ptr == text.data() + text.size();
}

std::pair<std::string_view, std::string_view> SplitOnce(std::string_view input, char delimiter) {
  const auto offset = input.find(delimiter);
  if (offset == std::string_view::npos) {
    return {input, {}};
  }
  return {input.substr(0, offset), input.substr(offset + 1)};
}

bool ParseOwner(std::string_view field, uint64_t *generation, uint64_t *kernel_owner) {
  const auto [first, second] = SplitOnce(field, ':');
  return field.find(':') != std::string_view::npos && ParseNumber(first, generation) &&
         ParseNumber(second, kernel_owner) && *generation > 0 &&
         field == folly::sformat("{}:{}", *generation, *kernel_owner);
}

utils::Status ValidateRange(const Range &range, Interval *result) {
  if (range.length != 0 && range.length > std::numeric_limits<uint64_t>::max() - range.start) {
    return utils::Status::InvalidArgument("POSIX lock range overflows");
  }
  if (range.type != Type::kRead && range.type != Type::kWrite && range.type != Type::kUnlock) {
    return utils::Status::InvalidArgument("unknown POSIX lock type");
  }
  if (range.pid < 0) {
    return utils::Status::InvalidArgument("negative POSIX holder PID");
  }
  *result = {range.start, range.length == 0 ? UINT64_MAX : range.start + range.length, range.type, range.pid};
  if (result->start == result->end) {
    return utils::Status::InvalidArgument("POSIX lock interval is empty");
  }
  return utils::Status::OK();
}

bool Overlaps(const Interval &a, const Interval &b) {
  return a.start < b.end && b.start < a.end;
}

utils::Status Decode(std::string_view data, bool flock, Holder *holder) {
  if (data.size() > kMaxValueBytes) {
    return utils::Status::Malformed("lock owner value exceeds maximum size");
  }
  if (flock) {
    if (data == "F1|S") {
      holder->flock = Flock::kShared;
    } else if (data == "F1|X") {
      holder->flock = Flock::kExclusive;
    } else {
      return utils::Status::Malformed("invalid flock record");
    }
    return utils::Status::OK();
  }
  if (!data.starts_with("P2|")) {
    return utils::Status::Malformed("invalid POSIX lock record version");
  }
  data.remove_prefix(3);
  if (data.empty() || data.back() == '|') {
    return utils::Status::Malformed("empty stored POSIX lock record");
  }
  while (!data.empty()) {
    const auto [entry, rest] = SplitOnce(data, '|');
    const auto [kind, tail] = SplitOnce(entry, ',');
    const auto [start, remaining] = SplitOnce(tail, ',');
    const auto [end, pid] = SplitOnce(remaining, ',');
    uint64_t first = 0;
    uint64_t last = 0;
    uint64_t holder_pid = 0;
    if ((kind != "R" && kind != "W") || remaining.find(',') == std::string_view::npos || !ParseNumber(start, &first) ||
        !ParseNumber(end, &last) || !ParseNumber(pid, &holder_pid) || holder_pid > INT32_MAX || first >= last) {
      return utils::Status::Malformed("malformed POSIX range");
    }
    if (!holder->ranges.empty() && (holder->ranges.back().end > first ||
                                    (holder->ranges.back().end == first &&
                                     holder->ranges.back().type == (kind == "R" ? Type::kRead : Type::kWrite) &&
                                     holder->ranges.back().pid == static_cast<int32_t>(holder_pid)))) {
      return utils::Status::Malformed("POSIX range set is not canonical");
    }
    holder->ranges.push_back({first, last, kind == "R" ? Type::kRead : Type::kWrite, static_cast<int32_t>(holder_pid)});
    if (holder->ranges.size() > kMaxIntervals) {
      return utils::Status::Malformed("POSIX range count exceeds maximum");
    }
    if (rest.empty()) {
      break;
    }
    data = rest;
  }
  return utils::Status::OK();
}

std::string Encode(const std::vector<Interval> &ranges) {
  std::string result = "P2";
  for (const auto &range : ranges) {
    result += folly::sformat("|{},{},{},{}", range.type == Type::kRead ? 'R' : 'W', range.start, range.end, range.pid);
  }
  return result;
}

utils::Status UpdateRanges(std::vector<Interval> *ranges, const Interval &request) {
  std::vector<Interval> next;
  next.reserve(ranges->size() + 2);
  // Remove the overlapped portion of this owner's old state before inserting
  // the requested mode; a conversion never keeps overlapping old modes.
  for (const auto &part : *ranges) {
    if (!Overlaps(part, request)) {
      next.push_back(part);
      continue;
    }
    if (part.start < request.start) {
      next.push_back({part.start, request.start, part.type, part.pid});
    }
    if (request.end < part.end) {
      next.push_back({request.end, part.end, part.type, part.pid});
    }
  }
  if (request.type != Type::kUnlock) {
    next.push_back(request);
  }
  std::sort(next.begin(), next.end(), [](const Interval &a, const Interval &b) { return a.start < b.start; });
  ranges->clear();
  for (const auto &part : next) {
    if (!ranges->empty() && ranges->back().end == part.start && ranges->back().type == part.type &&
        ranges->back().pid == part.pid) {
      ranges->back().end = part.end;
    } else {
      ranges->push_back(part);
    }
  }
  if (ranges->size() > kMaxIntervals || Encode(*ranges).size() > kMaxValueBytes) {
    return utils::Status::NoLocks("POSIX lock owner exceeds range budget");
  }
  return utils::Status::OK();
}

// WATCH every generation key together, then batch PTTL in one read-only Lua
// evaluation. An EXEC validates the watched snapshot, even for GETLK.
utils::Status ReadHolders(RedisKvTxn &txn, const redis::RedisKey &key, std::string_view hash,
                          uint64_t requester_generation, bool flock, std::vector<Holder> *holders, bool *saw_stale,
                          size_t *stored_fields = nullptr) {
  *saw_stale = false;
  uint64_t count = 0;
  auto status = txn.HLen(hash, &count);
  if (!status.ok()) {
    return status;
  }
  if (count > kMaxHolders) {
    return utils::Status::NoLocks("inode lock Hash exceeds the owner limit");
  }
  std::vector<std::pair<std::string, std::string>> entries;
  status = txn.HGetAll(hash, &entries);
  if (!status.ok()) {
    return status;
  }
  if (entries.size() > kMaxHolders) {
    return utils::Status::NoLocks("inode lock Hash changed beyond limit");
  }
  if (stored_fields != nullptr) {
    *stored_fields = entries.size();
  }
  std::vector<Holder> candidates;
  candidates.reserve(entries.size());
  std::vector<std::string> lease_keys{key.OwnerLease(requester_generation)};
  std::unordered_map<uint64_t, size_t> lease_positions{{requester_generation, 0}};
  for (const auto &[field, data] : entries) {
    (void)data;
    Holder holder{};
    if (!ParseOwner(field, &holder.generation, &holder.kernel_owner)) {
      return utils::Status::Malformed("invalid lock owner field");
    }
    if (!lease_positions.contains(holder.generation)) {
      lease_positions.emplace(holder.generation, lease_keys.size());
      lease_keys.push_back(key.OwnerLease(holder.generation));
    }
    candidates.push_back(std::move(holder));
  }
  std::vector<int64_t> ttl_values;
  status = txn.PttlBatch(lease_keys, &ttl_values);
  if (!status.ok()) {
    return status;
  }
  if (ttl_values[0] <= 0) {
    return utils::Status::Unavailable("requester Owner Lease is no longer valid");
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    auto &holder = candidates[i];
    const auto ttl = ttl_values[lease_positions.at(holder.generation)];
    if (ttl == -1) {
      return utils::Status::Malformed("holder Owner Lease has no mandatory expiry");
    }
    if (ttl >= 0) {
      status = Decode(entries[i].second, flock, &holder);
      if (!status.ok()) {
        return status;
      }
      holders->push_back(std::move(holder));
    } else {
      *saw_stale = true;
    }
  }
  return utils::Status::OK();
}

utils::Status SetState(RedisKvTxn &txn, std::string_view hash, std::string_view field, std::string_view data,
                       bool exists) {
  if (!data.empty()) {
    return txn.HSet(hash, field, data);
  }
  if (exists) {
    return txn.HDel(hash, field);
  }
  // Redis HDEL(nonexistent field) is not a write and cannot invalidate a
  // delayed old HSET's WATCH. An ephemeral same-Hash write fence is required.
  auto status = txn.HSet(hash, kFenceField, "1");
  if (!status.ok()) {
    return status;
  }
  status = txn.HDel(hash, field);
  if (!status.ok()) {
    return status;
  }
  return txn.HDel(hash, kFenceField);
}

}  // namespace

RedisLockAuthority::RedisLockAuthority(const RedisMetaConfig &config, std::string_view volume_name,
                                       RedisOwnerLease &owner_lease)
    : client_(config), key_(config.db, volume_name), owner_lease_(owner_lease) {
}

bool RedisLockAuthority::CanGrant() const {
  return owner_lease_.CanGrant(owner_lease_.generation());
}

utils::ThreadMutex &RedisLockAuthority::OwnerMutex(uint64_t inode_ino, uint64_t kernel_owner, bool flock) {
  // Per-owner stripes serialize different requests from the same Mount, but
  // never introduce a mount-global lock on unrelated inodes.
  return mutexes_[(inode_ino ^ (kernel_owner * 0x9e3779b97f4a7c15ULL) ^ (flock ? 17 : 0)) % mutexes_.size()];
}

utils::Status RedisLockAuthority::CheckLease(uint64_t generation) const {
  if (!owner_lease_.CanGrant(generation)) {
    return utils::Status::Unavailable("local Owner Lease is not valid for lock operations");
  }
  return utils::Status::OK();
}

void RedisLockAuthority::CleanupStale(std::string_view hash) {
  // The stale generation cannot be reissued. WATCH the inode Hash and each
  // candidate's lease before conditional physical removal; GC never grants.
  // Serialize reclamation for the same Hash, so two threads cannot repeatedly
  // overwrite each other's cursor continuation with an older page.
  std::lock_guard<utils::ThreadMutex> scan_lock(gc_mutexes_[std::hash<std::string_view>{}(hash) % gc_mutexes_.size()]);
  uint64_t start_cursor = 0;
  {
    std::lock_guard<utils::ThreadMutex> lock(gc_cursor_mutex_);
    if (const auto it = gc_cursors_.find(std::string(hash)); it != gc_cursors_.end()) {
      start_cursor = it->second;
    }
  }
  uint64_t next_cursor = start_cursor;
  const auto result = client_.Transact([&](RedisKvTxn &txn) {
    std::vector<std::pair<std::string, std::string>> entries;
    auto status = txn.HScan(hash, start_cursor, 128, &entries, &next_cursor);
    if (!status.ok()) {
      return status;
    }
    std::unordered_map<uint64_t, size_t> lease_positions;
    std::vector<std::string> lease_keys;
    std::vector<std::pair<std::string, uint64_t>> candidates;
    std::vector<std::string> dead_fields;
    // COUNT is only a Redis hint. Consume the entire returned page before
    // advancing; truncating it would permanently skip the remaining fields.
    for (size_t i = 0; i < entries.size(); ++i) {
      uint64_t generation = 0;
      uint64_t kernel_owner = 0;
      if (!ParseOwner(entries[i].first, &generation, &kernel_owner)) {
        continue;  // Unrecognizable fields cannot safely be classified stale.
      }
      if (!lease_positions.contains(generation)) {
        lease_positions.emplace(generation, lease_keys.size());
        lease_keys.push_back(key_.OwnerLease(generation));
      }
      candidates.emplace_back(entries[i].first, generation);
    }
    std::vector<int64_t> ttl_values;
    status = txn.PttlBatch(lease_keys, &ttl_values);
    if (!status.ok()) {
      return status;
    }
    for (const auto &[field, generation] : candidates) {
      if (ttl_values[lease_positions.at(generation)] == -2) {
        dead_fields.push_back(field);
      }
    }
    for (const auto &field : dead_fields) {
      status = txn.HDel(hash, field);
      if (!status.ok()) {
        return status;
      }
    }
    return utils::Status::OK();
  });
  if (!result.ok()) {
    return;  // Never advance based on an unverified/aborted transaction.
  }
  std::lock_guard<utils::ThreadMutex> lock(gc_cursor_mutex_);
  if (next_cursor == 0) {
    gc_cursors_.erase(std::string(hash));  // A complete scan cycle restarts at 0.
  } else {
    if (!gc_cursors_.contains(std::string(hash)) && gc_cursors_.size() >= kMaxCachedGcCursors) {
      gc_cursors_.erase(gc_cursors_.begin());  // Eviction is safe: GC is best effort.
    }
    gc_cursors_[std::string(hash)] = next_cursor;
  }
}

utils::Status RedisLockAuthority::SetPosix(uint64_t inode_ino, uint64_t kernel_owner, PosixRange request) {
  utils::ExpectInThreadDomain();
  Interval interval{};
  auto status = ValidateRange(request, &interval);
  if (!status.ok()) {
    return status;
  }
  const auto generation = owner_lease_.generation();
  const auto hash = key_.PosixLock(inode_ino);
  const auto field = folly::sformat("{}:{}", generation, kernel_owner);
  std::lock_guard<utils::ThreadMutex> lock(OwnerMutex(inode_ino, kernel_owner, false));
  for (int attempt = 0; attempt < kUncertainAttempts; ++attempt) {
    status = CheckLease(generation);
    if (!status.ok()) {
      return status;
    }
    bool saw_stale = false;
    status = client_.Transact([&](RedisKvTxn &txn) {
      std::vector<Holder> holders;
      size_t stored_fields = 0;
      auto status = ReadHolders(txn, key_, hash, generation, false, &holders, &saw_stale, &stored_fields);
      if (!status.ok()) {
        return status;
      }
      std::vector<Interval> owned;
      bool exists = false;
      for (const auto &holder : holders) {
        if (holder.generation == generation && holder.kernel_owner == kernel_owner) {
          exists = true;
          owned = holder.ranges;
        } else if (request.type != PosixType::kUnlock) {
          for (const auto &other : holder.ranges) {
            if (Overlaps(interval, other) && (request.type == PosixType::kWrite || other.type == PosixType::kWrite)) {
              return utils::Status::WouldBlock("conflicting POSIX record lock");
            }
          }
        }
      }
      status = UpdateRanges(&owned, interval);
      if (!status.ok()) {
        return status;
      }
      // HLEN == the cap may update an existing owner, but must never admit
      // a new field. The Hash is already WATCHed; no extra Redis RTT needed.
      if (!exists && !owned.empty() && stored_fields >= kMaxHolders) {
        return utils::Status::NoLocks("inode POSIX lock holder limit reached");
      }
      return SetState(txn, hash, field, owned.empty() ? std::string{} : Encode(owned), exists);
    });
    if (!status.IsOutcomeUnknown()) {
      if (saw_stale || status.ToErrno() == ENOLCK) {
        CleanupStale(hash);
      }
      return status.ok() ? CheckLease(generation) : status;
    }
  }
  return utils::Status::IOError("POSIX lock outcome could not be confirmed after bounded retries");
}

utils::Status RedisLockAuthority::GetPosix(uint64_t inode_ino, uint64_t kernel_owner, PosixRange request,
                                           std::optional<PosixConflict> *conflict) {
  utils::ExpectInThreadDomain();
  if (conflict == nullptr || request.type == PosixType::kUnlock) {
    return utils::Status::InvalidArgument("GETLK needs a conflict output and a requested lock mode");
  }
  Interval interval{};
  auto status = ValidateRange(request, &interval);
  if (!status.ok()) {
    return status;
  }
  const auto generation = owner_lease_.generation();
  status = CheckLease(generation);
  if (!status.ok()) {
    return status;
  }
  std::optional<PosixConflict> result;
  bool saw_stale = false;
  status = client_.Transact([&](RedisKvTxn &txn) {
    result.reset();
    std::vector<Holder> holders;
    auto status = ReadHolders(txn, key_, key_.PosixLock(inode_ino), generation, false, &holders, &saw_stale);
    if (!status.ok()) {
      return status;
    }
    for (const auto &holder : holders) {
      if (holder.generation == generation && holder.kernel_owner == kernel_owner) {
        continue;
      }
      for (const auto &other : holder.ranges) {
        if (Overlaps(interval, other) && (request.type == PosixType::kWrite || other.type == PosixType::kWrite)) {
          result = PosixConflict{other.start, other.end == UINT64_MAX ? 0 : other.end - other.start,
                                 holder.kernel_owner, other.type, other.pid};
          return utils::Status::OK();
        }
      }
    }
    return utils::Status::OK();
  });
  if (saw_stale || status.ToErrno() == ENOLCK) {
    CleanupStale(key_.PosixLock(inode_ino));
  }
  if (!status.ok()) {
    return status;
  }
  status = CheckLease(generation);
  if (status.ok()) {
    *conflict = result;
  }
  return status;
}

utils::Status RedisLockAuthority::SetFlock(uint64_t inode_ino, uint64_t kernel_owner, FlockType type) {
  utils::ExpectInThreadDomain();
  if (type != FlockType::kShared && type != FlockType::kExclusive && type != FlockType::kUnlock) {
    return utils::Status::InvalidArgument("invalid flock mode");
  }
  const auto generation = owner_lease_.generation();
  const auto hash = key_.FlockLock(inode_ino);
  const auto field = folly::sformat("{}:{}", generation, kernel_owner);
  std::lock_guard<utils::ThreadMutex> lock(OwnerMutex(inode_ino, kernel_owner, true));
  for (int attempt = 0; attempt < kUncertainAttempts; ++attempt) {
    auto status = CheckLease(generation);
    if (!status.ok()) {
      return status;
    }
    bool saw_stale = false;
    status = client_.Transact([&](RedisKvTxn &txn) {
      std::vector<Holder> holders;
      size_t stored_fields = 0;
      auto status = ReadHolders(txn, key_, hash, generation, true, &holders, &saw_stale, &stored_fields);
      if (!status.ok()) {
        return status;
      }
      bool exists = false;
      for (const auto &holder : holders) {
        if (holder.generation == generation && holder.kernel_owner == kernel_owner) {
          exists = true;
          continue;
        }
        if (type != FlockType::kUnlock && (type == FlockType::kExclusive || holder.flock == FlockType::kExclusive)) {
          return utils::Status::WouldBlock("conflicting BSD flock");
        }
      }
      if (!exists && type != FlockType::kUnlock && stored_fields >= kMaxHolders) {
        return utils::Status::NoLocks("inode flock holder limit reached");
      }
      return SetState(txn, hash, field,
                      type == FlockType::kUnlock ? "" : (type == FlockType::kShared ? "F1|S" : "F1|X"), exists);
    });
    if (!status.IsOutcomeUnknown()) {
      if (saw_stale || status.ToErrno() == ENOLCK) {
        CleanupStale(hash);
      }
      return status.ok() ? CheckLease(generation) : status;
    }
  }
  return utils::Status::IOError("flock outcome could not be confirmed after bounded retries");
}

}  // namespace swordfs::metadata
