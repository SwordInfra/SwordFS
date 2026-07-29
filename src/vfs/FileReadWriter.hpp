// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileReadWriter — per-open-file read/write over a chain of chunks.
// Owns the chunk deque and provides file-level I/O.

#pragma once

#include <folly/io/IOBuf.h>

#include <cstdint>
#include <deque>

#include "chunk/Chunk.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"

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

  FileReadWriter(metadata::IMetaEngine* meta,
                 storage::IDataEngine* data,
                 InodeID ino);

  /// Write |size| bytes at |off|, splitting across chunk boundaries.
  Status Write(const char* data, size_t size, off_t off);

  /// Read up to |size| bytes at |off|, merging buffer + storage.
  utils::Status Read(size_t size, off_t off, folly::IOBuf* out);

  /// Seal and upload all chunks.
  Status Flush();

  /// Find the chunk covering |off| for |ino|, or nullptr.
  chunk::Chunk* FindChunk(InodeID ino, off_t off);

 private:
  chunk::Chunk& GetOrCreateChunk(off_t off);

  metadata::IMetaEngine* meta_;
  storage::IDataEngine* data_;
  InodeID ino_;
  std::deque<chunk::Chunk> chunks_;
};

}  // namespace vfs
}  // namespace swordfs
