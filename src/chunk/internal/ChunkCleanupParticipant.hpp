// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "metadata/ChunkMetadata.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {
class IMetaEngine;
struct PendingDelete;
struct ReclaimWork;
}  // namespace swordfs::metadata

namespace swordfs::storage {
class IDataEngine;
}  // namespace swordfs::storage

namespace swordfs::chunk::internal {

// Final FileMetadata reachability hook staged by #319. The callback reports
// the authoritative ChunkID currently attached at one logical position. An
// empty optional means the position is a hole or its inode is no longer live.
using ChunkReachabilityProbeFn =
    std::function<utils::Status(metadata::InodeID, metadata::ChunkIndex, std::optional<metadata::ChunkID> *)>;

// Mechanism-owned destructive cleanup policy. Queue membership never grants
// delete authority: implementations must revalidate their authoritative
// metadata before touching physical data.
class ChunkCleanupParticipant {
 public:
  virtual ~ChunkCleanupParticipant() = default;

  // |completed| means the generic queue entry may be acknowledged. It can be
  // true after successful deletion or when revalidation proves that a stale
  // maintenance candidate should be discarded without deleting anything.
  virtual utils::Status DeletePending(const metadata::PendingDelete &work, bool *completed) = 0;
  virtual utils::Status DeleteReclaim(const metadata::ReclaimWork &work, bool *completed) = 0;
};

utils::Status CreateChunkCleanupParticipant(metadata::ChunkType chunk_type, uint64_t chunk_size,
                                            metadata::ChunkMetadataPtr chunk_metadata, metadata::IMetaEngine *meta,
                                            storage::IDataEngine *data, ChunkReachabilityProbeFn reachability_probe,
                                            std::unique_ptr<ChunkCleanupParticipant> *out);

}  // namespace swordfs::chunk::internal
