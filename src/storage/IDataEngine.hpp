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
//   USE Engine (enterprise) — in-place random overwrite via NVMe-oF.
//
// The two engines share the same chunk-level metadata representation.
// The difference — immutable slices vs in-place overwrites — is
// encapsulated within each engine's implementation.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::storage {

/// Engine capability limits.
struct DataEngineLimits {
  /// Maximum chunk size in bytes.  The upper layer (VfsImpl) MUST
  /// segment writes into chunks no larger than this value.
  size_t max_chunk_size = 64 * 1024 * 1024;  // 64 MiB

  /// Whether the engine supports multipart uploads.
  bool supports_multipart = false;

  /// Whether the engine supports random overwrite (in-place mutation).
  /// Object storage does not; USE does.
  bool supports_overwrite = false;
};

/// Abstract data-plane engine.
///
/// Chunks are addressed by opaque string keys whose format is defined
/// by the concrete engine (e.g. "chunks/0/1/23_0_4" for object storage).
/// The engine itself has no knowledge of inodes or file-system concepts.
class IDataEngine {
 public:
  virtual ~IDataEngine() = default;

  /// Return the engine's capability limits.
  virtual DataEngineLimits Limits() const = 0;

  /// Check whether a chunk exists and return its size.
  /// @param key  chunk key.
  /// @param size receives the object size if it exists (may be null).
  /// @return true if the chunk exists.
  virtual bool Head(std::string_view key, size_t* size) = 0;

  /// Write a chunk.  Returns the key that can be used to read it back.
  /// For object-storage engines the key is the object path; for USE it
  /// is a chunk locator.
  virtual Status Put(std::string_view key, std::string_view data) = 0;

  /// Read all or part of a chunk.
  ///
  /// @param key    chunk key (opaque, engine-defined).
  /// @param out    receives the chunk data.
  /// @param offset byte offset within the chunk (0 = from start).
  /// @param size   number of bytes to read (0 = until end).
  virtual Status Get(std::string_view key, std::string* out,
                     size_t offset = 0, size_t size = 0) = 0;

  /// Delete a chunk (called by the garbage collector).
  virtual Status Delete(std::string_view key) = 0;
};

}  // namespace swordfs::storage
