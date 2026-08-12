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

#include "chunk/Chunk.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs {

namespace metadata {
class IMetaEngine;
}

namespace storage {
class IDataEngine;
}

namespace vfs {

// ────────────────────────────────────────────────────────────────
// FileChunkManager — thread-safe manager of inode chunks (both dirty and
// flushed).  All synchronisation is internal.  Flushed chunks are
// not removed so reads can reach them directly via Chunk::Read().
// ────────────────────────────────────────────────────────────────

class FileChunkManager {
 public:
  using Map = folly::F14FastMap<metadata::ChunkIndex, chunk::Chunk>;

  explicit FileChunkManager(metadata::InodeID ino) : ino_(ino) {}

  /// Get the chunk at |idx|.  If not in the map, creates and
  /// initializes it.  Returns nullptr on error or when
  /// create_if_missing=false and no flushed data exists.
  /// The pointer is valid only until the next non-const call.
  chunk::Chunk *Get(metadata::ChunkIndex idx, bool create_if_missing);

  /// Return the next chunk that has data and is not yet sealed.
  /// Seals it before returning.  Returns nullptr when all chunks
  /// have been flushed.  The pointer is valid until the next
  /// non-const call.
  chunk::Chunk *GetNextFlushable();

 private:
  metadata::InodeID ino_;
  mutable std::mutex mutex_;
  Map chunks_;
};

class FileReadWriter {
 public:
  using InodeID = metadata::InodeID;

  FileReadWriter(InodeID ino);

  /// Return the inode this writer is bound to.
  InodeID ino() const { return ino_; }

  /// Write the contents of |buf| at |off|, splitting across chunk boundaries.
  utils::Status Write(const folly::IOBuf &buf, off_t off);

  /// Read up to |size| bytes at |off| into |out|.
  /// |out| must be an empty IOBuf with capacity >= |size|.
  /// On success, out->length() reflects bytes actually read.
  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  /// Seal and upload all dirty chunks, then register them with the
  /// metadata engine.
  utils::Status Flush();

 private:
  InodeID ino_;
  size_t chunk_size_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  FileChunkManager chunks_;
};

}  // namespace vfs
}  // namespace swordfs
