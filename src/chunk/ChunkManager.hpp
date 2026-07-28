// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// ChunkManager — manages chunk-level I/O: write buffering, chunk seal
// and upload, and chunk-aware read (merging buffer data with storage).

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>
#include <deque>
#include <string>

#include "chunk/Chunk.hpp"
#include "chunk/WriteBuf.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs {

namespace metadata {
class IMetaEngine;
}

namespace storage {
class IDataEngine;
}

namespace chunk {

class ChunkManager {
 public:
  using Status = utils::Status;
  using InodeID = metadata::InodeID;

  /// Construct with the metadata and data engines that back all I/O.
  /// Both pointers must remain valid for the lifetime of this object.
  ChunkManager(metadata::IMetaEngine* meta, storage::IDataEngine* data);

  // ──────────────────────────────────────────────────────────────
  // Write path
  // ──────────────────────────────────────────────────────────────

  /// Append |size| bytes from |data| at file offset |off|.  When the
  /// current chunk fills up it is automatically sealed and a new
  /// chunk is created for the remainder.
  Status Write(uint64_t fh, InodeID ino, const char* data, size_t size,
               off_t off);

  /// Seal and upload ALL chunks for |fh|.  Called on flush / fsync /
  /// release.
  Status Flush(uint64_t fh);

  /// Upload sealed chunks and discard the chunk chain (called on
  /// file release).  The meta engine's Release is still the caller's
  /// responsibility.
  Status Release(uint64_t fh);

  // ──────────────────────────────────────────────────────────────
  // Read path
  // ──────────────────────────────────────────────────────────────

  /// Read up to |size| bytes starting at |off|.
  ///
  /// Unflushed write-buffer data takes precedence over stored chunks.
  /// Appends the resulting data to |out|.  Returns OK on success
  /// (including short reads / EOF).
  Status Read(InodeID ino, size_t size, off_t off, folly::IOBuf* out);

 private:
  /// Get or create the chunk deque for |fh|.
  std::deque<Chunk>& GetChunks(uint64_t fh);

  /// Upload a single sealed chunk to the data engine.
  Status UploadChunk(Chunk& c);

  /// Find the chunk that contains |off|, or nullptr.
  Chunk* FindChunk(InodeID ino, off_t off);

  metadata::IMetaEngine* meta_;  // non-owning
  storage::IDataEngine* data_;   // non-owning
  folly::F14FastMap<uint64_t, std::deque<Chunk>> chunks_;
};

}  // namespace chunk
}  // namespace swordfs
