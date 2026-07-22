// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// IMetadataEngine — abstract interface for metadata storage backends.
//
// The engine layer operates on raw byte-string keys and values. It has no
// knowledge of inodes, directory entries, chunks, or any other file-system
// concept. All file-system semantics live in the meta-operations layer built
// on top of this interface.
//
// Key design decisions (from JuiceFS reference):
//   - Flat key-value namespace with prefix-based encoding for sorted scans
//   - Optimistic concurrency control (OCC) for transactions
//   - Batch ID allocation via IncrBy to reduce transaction contention
//   - Single-key operations are non-transactional for read performance

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "meta/EngineError.hpp"

namespace swordfs::meta {

// Forward declaration.
class IMetadataEngine;

/// Transaction handle — returned by IMetadataEngine::BeginTxn().
///
/// A transaction captures a snapshot of the store at begin time and buffers
/// writes locally. On Commit(), the engine verifies no write-write conflicts
/// via optimistic concurrency control and atomically applies all buffered
/// mutations.
///
/// The caller must call Commit() exactly once (or destroy the handle to
/// discard). Transactions are not reusable after Commit().
class ITransaction {
 public:
  virtual ~ITransaction() = default;

  /// Read a key's value within this transaction's snapshot.
  /// Returns std::nullopt if the key does not exist.
  virtual std::optional<std::string> Get(std::string_view key) = 0;

  /// Write a key-value pair into the transaction's write buffer.
  /// The write is not visible to other transactions until Commit().
  virtual EngineError Set(std::string_view key, std::string_view value) = 0;

  /// Delete a key within this transaction.
  virtual EngineError Delete(std::string_view key) = 0;

  /// Atomically increment a counter key by delta.
  /// Used for batch ID allocation: e.g., IncrBy("nextInode", 1024).
  virtual EngineError IncrBy(std::string_view key, int64_t delta) = 0;

  /// Commit all buffered writes to the store.
  ///
  /// Returns kOk on success, kConflict if a write-write conflict was detected.
  /// On kConflict, the caller should retry the entire transaction from the
  /// beginning (re-read, re-compute, re-commit).
  virtual EngineError Commit() = 0;
};

/// Abstract interface for a transactional key-value metadata store.
///
/// Implementations:
///   - MemEngine: in-memory OCC engine (Phase 1)
///   - RedisEngine: Redis-backed engine (future)
///   - TiKVEngine: TiKV-backed engine (future)
class IMetadataEngine {
 public:
  virtual ~IMetadataEngine() = default;

  /// Callback type for Scan(): return false to stop iteration early.
  using ScanCallback =
      std::function<bool(std::string_view key, std::string_view value)>;

  // ── Single-key operations (non-transactional, best-effort reads) ──────

  /// Read a single key. Returns std::nullopt if not found.
  virtual std::optional<std::string> Get(std::string_view key) = 0;

  /// Write a single key-value pair atomically (no transaction needed).
  virtual EngineError Set(std::string_view key, std::string_view value) = 0;

  /// Delete a single key atomically.
  virtual EngineError Delete(std::string_view key) = 0;

  // ── Range scan ────────────────────────────────────────────────────────

  /// Scan keys in [begin, end). The callback receives each key-value pair;
  /// return false to stop iteration early.
  virtual void Scan(std::string_view begin, std::string_view end,
                    ScanCallback callback) = 0;

  /// Check whether any key with the given prefix exists.
  virtual bool Exists(std::string_view prefix) = 0;

  // ── Transactions ──────────────────────────────────────────────────────

  /// Begin a new transaction with a snapshot of the current store state.
  virtual std::unique_ptr<ITransaction> BeginTxn() = 0;

  // ── ID allocation ─────────────────────────────────────────────────────

  /// Allocate the next unique ID with batch pre-allocation (1024 at a time).
  /// Thread-safe. The meta-operations layer can also use IncrBy() on counter
  /// keys inside transactions for transactional ID allocation.
  virtual uint64_t NextId() = 0;
};

}  // namespace swordfs::meta
