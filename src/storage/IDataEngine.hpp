// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// IDataEngine — abstract interface for the SwordFS data plane.
//
// SwordFS files are expressed as sequences of chunks.  The metadata
// plane maps each file to its ordered list of chunk identifiers.
// How chunks are stored and accessed is determined by the data-plane
// engine, which implements this interface.
//
//   Object Storage Engine (open-source) — chunks stored as immutable
//       objects in S3-compatible storage.
//
// Additional backends implement the same interface.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

using Status = swordfs::utils::Status;

namespace swordfs::storage {

/// Engine capability flags.
struct DataEngineLimits {
  /// Whether the engine supports multipart uploads.
  bool supports_multipart = false;
};

/// Abstract data-plane engine.
///
/// Chunks are addressed by opaque string keys derived by the chunk/data-layout
/// layer (currently "<ino>/<chunk-index>/<revision>"). The engine itself has
/// no knowledge of inodes, revisions, or other file-system concepts.
///
/// Execution-domain contract: construction/destruction and Initialize() are
/// POSIX-thread lifecycle operations; Head/Put/Get/Delete are fiber-domain
/// runtime operations. Implementations MUST dispatch blocking I/O (e.g. SDK
/// calls to object storage) to a background thread pool and suspend the
/// calling fiber rather than blocking the EventBase thread. See S3DataEngine
/// for the recommended pattern: an explicit BlockingExecutor::RunFromFiber()
/// transition to a POSIX worker thread.
class IDataEngine {
 public:
  virtual ~IDataEngine() = default;

  /// Initialize the data backend and its runtime resources.
  virtual Status Initialize() = 0;

  /// Return the engine's capability limits.
  virtual DataEngineLimits Limits() const = 0;

  /// Check whether a chunk exists and return its size.
  /// @param key  chunk key.
  /// @param size receives the object size if it exists (may be null).
  /// @return true if the chunk exists.
  virtual bool Head(std::string_view key, size_t *size) = 0;

  /// Write a complete immutable chunk object to the storage backend. Takes
  /// ownership of |data|. An OK result is the data-plane publication barrier:
  /// the entire object must be atomically readable under |key| before the
  /// caller may publish metadata that references it. A partial object must
  /// never be reported as a successful write.
  virtual Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) = 0;

  /// Read all or part of a chunk. Data is appended directly to |out|, which
  /// must have tailroom() >= the expected size. A bounded request (|size| > 0)
  /// may append fewer bytes when the backing object or range is short, so
  /// callers that require exact-length semantics must validate the appended
  /// length before exposing the output. A zero |size| requests the remainder
  /// of the object.
  virtual Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) = 0;

  /// Delete a chunk (called by the garbage collector).
  ///
  /// Deletion MUST be idempotent from the caller's perspective: deleting an
  /// already-absent key returns OK. Durable metadata cleanup may replay the
  /// same immutable object identity after a timeout, process restart, or
  /// ambiguous acknowledgement, and must never need a separate existence
  /// check to make retry safe.
  virtual Status Delete(std::string_view key) = 0;
};

}  // namespace swordfs::storage
