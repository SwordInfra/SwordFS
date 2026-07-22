// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "meta/MemEngine.hpp"

#include <algorithm>
#include <functional>

namespace swordfs::meta {

// ── MemEngine ──────────────────────────────────────────────────────────────

MemEngine::MemEngine() = default;

std::optional<std::string> MemEngine::Get(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.find(key);
  if (it == store_.end()) {
    return std::nullopt;
  }
  return it->second.value;
}

EngineError MemEngine::Set(std::string_view key, std::string_view value) {
  std::lock_guard<std::mutex> lock(mu_);
  std::string key_str(key);
  auto it = store_.find(key_str);
  if (it == store_.end()) {
    store_.emplace(std::move(key_str), MemItem{std::string(value), 1});
  } else {
    it->second.value = value;
    it->second.version++;
  }
  return EngineError::kOk;
}

EngineError MemEngine::Delete(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  store_.erase(std::string(key));
  return EngineError::kOk;
}

void MemEngine::Scan(std::string_view begin, std::string_view end,
                     ScanCallback callback) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = store_.lower_bound(begin);
  auto end_it = end.empty() ? store_.end() : store_.lower_bound(end);

  for (; it != end_it; ++it) {
    if (!callback(it->first, it->second.value)) {
      break;
    }
  }
}

bool MemEngine::Exists(std::string_view prefix) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.lower_bound(prefix);
  if (it == store_.end()) return false;
  // Check that the found key actually starts with the prefix.
  return it->first.compare(0, prefix.size(), prefix) == 0;
}

std::unique_ptr<ITransaction> MemEngine::BeginTxn() {
  return std::make_unique<MemTxn>(this);
}

uint64_t MemEngine::NextId() {
  // Batch pre-allocation: refill 1024 IDs at a time to reduce contention
  // on the global counter. Same pattern as JuiceFS freeInodes/freeSlices.
  std::lock_guard<std::mutex> lock(mu_);
  if (id_pool_ == 0) {
    next_id_ += kIdBatchSize;
    id_pool_ = kIdBatchSize;
  }
  id_pool_--;
  return next_id_ - id_pool_;
}

EngineError MemEngine::CommitTxn(
    const std::unordered_map<std::string, uint64_t>& observed,
    const std::map<std::string, std::string>& buffer,
    const std::map<std::string, bool>& deletes,
    const std::unordered_map<std::string, int64_t>& incrs) {
  std::lock_guard<std::mutex> lock(mu_);

  // Phase 1: OCC validation — check that no observed key has had its version
  // incremented since the transaction began.
  for (const auto& [key, observed_ver] : observed) {
    auto it = store_.find(key);
    if (it != store_.end()) {
      if (it->second.version > observed_ver) {
        return EngineError::kConflict;
      }
    } else {
      // Key was deleted after we observed it (observed_ver > 0 means it existed).
      if (observed_ver != 0) {
        return EngineError::kConflict;
      }
    }
  }

  // Phase 2: Apply buffered writes.
  for (const auto& [key, value] : buffer) {
    auto it = store_.find(key);
    if (it == store_.end()) {
      store_.emplace(key, MemItem{value, 1});
    } else {
      it->second.value = value;
      it->second.version++;
    }
  }

  // Phase 3: Apply deletes.
  for (const auto& [key, _] : deletes) {
    store_.erase(key);
  }

  // Phase 4: Apply increments.
  for (const auto& [key, delta] : incrs) {
    auto it = store_.find(key);
    if (it == store_.end()) {
      // Initialize counter to delta value.
      store_.emplace(key, MemItem{std::to_string(delta), 1});
    } else {
      int64_t current = std::stoll(it->second.value);
      it->second.value = std::to_string(current + delta);
      it->second.version++;
    }
  }

  return EngineError::kOk;
}

// ── MemTxn ─────────────────────────────────────────────────────────────────

MemTxn::MemTxn(MemEngine* engine) : engine_(engine) {}

std::optional<std::string> MemTxn::Get(std::string_view key) {
  std::string key_str(key);

  // Check write buffer first — reads should see our own writes.
  auto buf_it = buffer_.find(key_str);
  if (buf_it != buffer_.end()) {
    return buf_it->second;
  }

  // Check if we deleted this key in this transaction.
  if (deletes_.count(key_str)) {
    return std::nullopt;
  }

  // Read from the engine store (under lock) and record the version.
  std::lock_guard<std::mutex> lock(engine_->mu_);
  auto it = engine_->store_.find(key_str);
  if (it == engine_->store_.end()) {
    // Record that we observed the absence of this key (version 0).
    observed_.emplace(std::move(key_str), 0);
    return std::nullopt;
  }

  observed_.emplace(std::move(key_str), it->second.version);
  return it->second.value;
}

EngineError MemTxn::Set(std::string_view key, std::string_view value) {
  buffer_[std::string(key)] = value;
  return EngineError::kOk;
}

EngineError MemTxn::Delete(std::string_view key) {
  std::string key_str(key);
  buffer_.erase(key_str);  // Remove any pending write for this key.
  deletes_[std::move(key_str)] = true;
  return EngineError::kOk;
}

EngineError MemTxn::IncrBy(std::string_view key, int64_t delta) {
  std::string key_str(key);
  auto it = incrs_.find(key_str);
  if (it == incrs_.end()) {
    incrs_.emplace(std::move(key_str), delta);
  } else {
    it->second += delta;
  }
  return EngineError::kOk;
}

EngineError MemTxn::Commit() {
  if (committed_) {
    return EngineError::kOk;  // Idempotent.
  }
  committed_ = true;

  // Snapshot observed keys (they must survive until commit validation).
  EngineError result =
      engine_->CommitTxn(observed_, buffer_, deletes_, incrs_);

  // Clear buffers regardless of result (transaction is done).
  buffer_.clear();
  deletes_.clear();
  incrs_.clear();

  return result;
}

}  // namespace swordfs::meta
