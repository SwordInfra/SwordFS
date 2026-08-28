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
/// Chunks are addressed by opaque string keys whose format is defined
/// by the concrete engine (e.g. "chunks/0/1/23_0_4" for object storage).
/// The engine itself has no knowledge of inodes or file-system concepts.
///
/// @important  Implementations MUST dispatch blocking I/O
/// (e.g. SDK calls to object storage) to a background thread pool
/// and suspend the calling fiber rather than blocking the OS thread.
/// Blocking the thread starves all other fibers on the same
/// EventBase.  See S3DataEngine for the recommended pattern
/// (FiberThreadPool + folly::fibers::Baton).
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

  /// Write a chunk to the storage backend.  Takes ownership of |data|.
  virtual Status Put(std::string_view key,
                     std::unique_ptr<folly::IOBuf> data) = 0;

  /// Read all or part of a chunk.  Data is written directly into
  /// |out|, which must have tailroom() >= expected size.
  virtual Status Get(std::string_view key,
                     size_t offset, size_t size,
                     folly::IOBuf* out) = 0;

  /// Delete a chunk (called by the garbage collector).
  virtual Status Delete(std::string_view key) = 0;
};

}  // namespace swordfs::storage
