// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// ChunkManager — manages chunk-level I/O: write buffering, chunk flush,
// and chunk-aware read (merging unflushed buffer data with stored chunks).

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>
#include <string>

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

  /// Append |size| bytes from |data| at file offset |off| to the
  /// in-memory write buffer identified by |fh|.  If the buffer
  /// reaches max_chunk_size a non-forced flush is performed
  /// automatically.  Returns the status of any automatic flush.
  Status Write(uint64_t fh, InodeID ino, const char* data, size_t size,
               off_t off);

  /// Flush the write buffer for |fh| to the data engine.
  ///
  /// When |force| is true the entire buffer is flushed regardless of
  /// size.  When false only max_chunk_size bytes are flushed and the
  /// remainder is kept for the next chunk.
  Status Flush(uint64_t fh, bool force);

  /// Discard the write buffer for |fh| (called on file close).
  void RemoveBuf(uint64_t fh);

  // ──────────────────────────────────────────────────────────────
  // Read path
  // ──────────────────────────────────────────────────────────────

  /// Read up to |size| bytes starting at |off|.
  ///
  /// Unflushed write-buffer data takes precedence over stored chunks.
  /// Appends the resulting data to |out|.  Returns OK on success
  /// (including short reads / EOF).
  Status Read(InodeID ino, size_t size, off_t off, std::string* out);

 private:
  /// Find the write buffer for |ino| that has unflushed data, or
  /// return nullptr if none exists.
  WriteBuf* FindWriteBuf(InodeID ino);

  metadata::IMetaEngine* meta_;  // non-owning
  storage::IDataEngine* data_;   // non-owning
  folly::F14FastMap<uint64_t, WriteBuf> write_bufs_;
};

}  // namespace chunk
}  // namespace swordfs
