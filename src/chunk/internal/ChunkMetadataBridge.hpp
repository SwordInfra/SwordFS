// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "metadata/IChunkIndexTxn.hpp"
#include "metadata/types/Reclaim.hpp"
#include "metadata/types/Volume.hpp"

namespace swordfs::chunk::internal {

// Transitional callback bridge for the common-authority metadata transactions
// that #317/#318/#319 progressively retire. It deliberately has no runtime
// chunk-construction or physical-delete surface.
class ChunkMetadataBridge {
 public:
  virtual ~ChunkMetadataBridge() = default;

  virtual utils::Status LoadPublished(metadata::IChunkIndexReader &reader, metadata::InodeID ino,
                                      const metadata::SwordFsChunk &head, std::string *private_snapshot) const = 0;
  virtual utils::Status Publish(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                const std::optional<metadata::SwordFsChunk> &expected,
                                const metadata::SwordFsChunk &replacement,
                                const metadata::ChunkPublishIntent &intent) const = 0;
  virtual utils::Status Truncate(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                 const std::vector<metadata::ChunkIndexChange> &changes) const = 0;
  virtual utils::Status PrepareReclaim(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                       const std::vector<metadata::SwordFsChunk> &heads) const = 0;

  virtual utils::Status FreezePendingDelete(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                            const metadata::SwordFsChunk &head, uint64_t chunk_size,
                                            metadata::PendingDelete *out) const = 0;
  virtual utils::Status FreezeRejectedPublication(metadata::InodeID ino, const metadata::SwordFsChunk &replacement,
                                                  const metadata::ChunkPublishIntent &intent, uint64_t chunk_size,
                                                  metadata::PendingDelete *out) const = 0;
  virtual utils::Status FreezeReclaim(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                      const std::vector<metadata::SwordFsChunk> &heads, uint64_t chunk_size,
                                      metadata::ReclaimWork *out) const = 0;
};

utils::Status CreateChunkMetadataBridge(metadata::ChunkType chunk_type, std::unique_ptr<ChunkMetadataBridge> *out);

}  // namespace swordfs::chunk::internal
