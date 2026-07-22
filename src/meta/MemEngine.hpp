// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// MemEngine — in-memory transactional key-value store with optimistic
// concurrency control (OCC).
//
// Storage: std::map (red-black tree) for ordered key-value storage.
// Concurrency: Snapshot Isolation via versioned items + write-buffer +
//               commit-time version check. Single mutex serializes commits.
// ID allocation: Batch pre-allocation (1024 IDs at a time) via NextId().

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "meta/IMetadataEngine.hpp"

namespace swordfs::meta {

/// Versioned key-value item.
struct MemItem {
  std::string value;
  uint64_t version = 0;  // Incremented on every write.
};

/// In-memory OCC engine implementation.
class MemEngine : public IMetadataEngine {
 public:
  MemEngine();

  // ── IMetadataEngine interface ─────────────────────────────────────────

  std::optional<std::string> Get(std::string_view key) override;
  EngineError Set(std::string_view key, std::string_view value) override;
  EngineError Delete(std::string_view key) override;

  void Scan(std::string_view begin, std::string_view end,
            ScanCallback callback) override;
  bool Exists(std::string_view prefix) override;

  std::unique_ptr<ITransaction> BeginTxn() override;
  uint64_t NextId() override;

 private:
  friend class MemTxn;

  /// Apply all buffered writes from a transaction after OCC validation.
  /// Called by MemTxn::Commit().
  EngineError CommitTxn(
      const std::unordered_map<std::string, uint64_t>& observed,
      const std::map<std::string, std::string>& buffer,
      const std::map<std::string, bool>& deletes,
      const std::unordered_map<std::string, int64_t>& incrs);

  std::mutex mu_;
  std::map<std::string, MemItem, std::less<>> store_;

  // ID batch pre-allocation.
  uint64_t next_id_ = 1;    // Next ID to hand out after current batch.
  uint64_t id_pool_ = 0;     // IDs remaining in the current batch.
  static constexpr uint64_t kIdBatchSize = 1024;
};

/// Transaction implementation for MemEngine.
class MemTxn : public ITransaction {
 public:
  explicit MemTxn(MemEngine* engine);
  ~MemTxn() override = default;

  std::optional<std::string> Get(std::string_view key) override;
  EngineError Set(std::string_view key, std::string_view value) override;
  EngineError Delete(std::string_view key) override;
  EngineError IncrBy(std::string_view key, int64_t delta) override;
  EngineError Commit() override;

 private:
  MemEngine* engine_;

  // Observed versions at read time (key → version). Used for OCC validation.
  std::unordered_map<std::string, uint64_t> observed_;

  // Write buffer — writes are buffered locally until commit.
  std::map<std::string, std::string> buffer_;

  // Delete buffer — keys to delete on commit.
  std::map<std::string, bool> deletes_;

  // Increment buffer — key → delta.
  std::unordered_map<std::string, int64_t> incrs_;

  bool committed_ = false;
};

}  // namespace swordfs::meta
