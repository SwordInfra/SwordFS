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

  static ChunkManager &Instance();

  /// Initialise the singleton.  Must be called once before any other
  /// method.  Both pointers must remain valid for the lifetime of the
  /// process.
  void Init(metadata::IMetaEngine *meta, storage::IDataEngine *data,
            size_t chunk_size);

  /// Reset all state (for unit testing).
  void ResetForTesting();

  /// Get or create the chunk covering file offset |off|.
  Chunk &GetOrCreateChunk(uint64_t fh, InodeID ino, off_t off);

  /// Seal all writing chunks and upload them to the data engine.
  Status Flush(uint64_t fh);

  /// Flush and then discard the chunk chain.
  Status Release(uint64_t fh);

  /// Find the chunk covering |off| for the given inode, or nullptr.
  Chunk *FindChunk(InodeID ino, off_t off);

 private:
  std::deque<Chunk> &GetChunks(uint64_t fh);

  metadata::IMetaEngine *meta_;  // non-owning
  storage::IDataEngine *data_;   // non-owning
  size_t chunk_size_;
  folly::F14FastMap<uint64_t, std::deque<Chunk>> chunks_;
};

}  // namespace chunk
}  // namespace swordfs
