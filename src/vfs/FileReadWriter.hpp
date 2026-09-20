// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileReadWriter — per-inode read/write over a chain of chunks.
// Shared across all file handles that reference the same inode so that
// writes from one handle are visible to reads on another (before flush).
//
// Writes are buffered in a per-inode dirty deque; Flush seals, uploads,
// and registers chunk metadata with the metadata engine so that
// subsequent reads can locate the data via storage.

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "chunk/Chunk.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs {

namespace metadata {
class IMetaEngine;
}

namespace vfs {

// ────────────────────────────────────────────────────────────────
// FileChunkManager — thread-safe manager of inode chunks (both dirty and
// flushed).  All synchronisation is internal.  Flushed chunks are
// not removed so reads can reach them directly via Chunk::Read().
// ────────────────────────────────────────────────────────────────

class FileChunkManager {
 public:
  using Map = folly::F14FastMap<metadata::ChunkIndex, std::shared_ptr<chunk::Chunk>>;

  explicit FileChunkManager(metadata::InodeID ino) : ino_(ino) {
  }

  /// Get the chunk at |idx|. If not in the map, creates and initializes it.
  /// A successful lookup with |*out == nullptr| means no materialized chunk
  /// exists and |create_if_missing| is false. Initialization failures are
  /// returned as Status and are never encoded as a null chunk.
  /// The shared pointer keeps the chunk alive if the map is changed.
  utils::Status Get(metadata::ChunkIndex idx, bool create_if_missing, std::shared_ptr<chunk::Chunk> *out);

  /// Snapshot chunks with pending data, including sealed chunks from a
  /// previous failed flush. The caller may attempt each snapshot entry once
  /// without repeatedly selecting the same failed chunk.
  std::vector<std::shared_ptr<chunk::Chunk>> GetFlushable();

  /// Apply a file-size change to cached chunks. A partial boundary chunk keeps
  /// only its surviving prefix; chunks wholly beyond EOF are dropped locally.
  /// Authoritative metadata owns best-effort object cleanup registration.
  void TruncateToSize(size_t size, size_t chunk_size);

 private:
  metadata::InodeID ino_;
  mutable utils::FiberMutex mutex_;
  Map chunks_;
};

class FileReadWriter {
 public:
  using InodeID = metadata::InodeID;

  FileReadWriter(InodeID ino);

  /// Write the contents of |buf| at |off|, splitting across chunk boundaries.
  utils::Status Write(const folly::IOBuf &buf, off_t off);

  /// Read up to |size| bytes at |off| into |out|.
  /// |out| must be an empty IOBuf with capacity >= |size|.
  /// On success, out->length() reflects bytes actually read.
  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  /// Seal and upload all dirty chunks, then register them with the
  /// metadata engine.
  utils::Status Flush();

  /// Truncate to |size| bytes.  Updates chunk metadata in the metadata
  /// engine and drops cached chunks.  Used by O_TRUNC (size=0).
  utils::Status Truncate(size_t size);

  /// Apply setattr while coordinating size changes with the local chunk
  /// cache so pending writes cannot be republished past a successful truncate.
  utils::Status SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields, metadata::SwordFsInode *out);

 private:
  InodeID ino_;
  size_t chunk_size_;
  metadata::IMetaEngine *meta_;
  // Coordinate operations for one inode. Reads may proceed concurrently;
  // writes, flushes, and truncates take exclusive ownership so they cannot
  // race chunk state transitions. Fiber-aware waiting never blocks the
  // EventBase driver thread.
  mutable utils::FiberRWMutex operation_mutex_;
  FileChunkManager chunks_;
};

}  // namespace vfs
}  // namespace swordfs
