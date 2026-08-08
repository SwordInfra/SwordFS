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

#include <cstdint>

#include <folly/container/F14Map.h>

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

class FileReadWriter {
 public:
  using Status = utils::Status;
  using InodeID = metadata::InodeID;

  FileReadWriter(InodeID ino, size_t chunk_size,
                 metadata::IMetaEngine* meta, storage::IDataEngine* data);

  /// Return the inode this writer is bound to.
  InodeID ino() const { return ino_; }

  /// Write the contents of |buf| at |off|, splitting across chunk boundaries.
  Status Write(const folly::IOBuf& buf, off_t off);

  /// Read up to |size| bytes at |off| into |out|.
  /// |out| must be an empty IOBuf with capacity >= |size|.
  /// On success, out->length() reflects bytes actually read.
  Status Read(size_t size, off_t off, folly::IOBuf* out);

  /// Seal and upload all dirty chunks, then register them with the
  /// metadata engine.
  Status Flush();

 private:
  /// Find a dirty chunk covering |off|, or nullptr.
  chunk::Chunk* FindDirtyChunk(off_t off);

  InodeID ino_;
  size_t chunk_size_;
  metadata::IMetaEngine* meta_;   // non-owning
  storage::IDataEngine* data_;    // non-owning
  folly::F14FastMap<metadata::ChunkIndex, chunk::Chunk> dirty_chunks_;
};

}  // namespace vfs
}  // namespace swordfs
