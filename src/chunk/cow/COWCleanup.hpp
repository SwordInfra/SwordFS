// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chunk/internal/ChunkCleanupParticipant.hpp"
#include "metadata/ChunkMetadata.hpp"
#include "metadata/ChunkSizePlan.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::chunk::cow {

enum class COWCleanupKind : uint32_t {
  kRevision = 1,
  kDetachedChunk = 2,
  kDetachedReclaim = 3,
};

struct COWRef {
  metadata::InodeID ino = 0;
  metadata::SwordFsChunk descriptor;
  std::string key;

  bool operator==(const COWRef &) const = default;
};

// These codecs belong to the COW implementation. The generic
// reclaim types persist their output as opaque bytes.
utils::Status FreezeCOWDelete(metadata::InodeID ino, const metadata::SwordFsChunk &chunk, uint64_t chunk_size,
                              metadata::PendingDelete *out);
utils::Status FreezeCOWReclaim(metadata::InodeID ino, const std::vector<metadata::SwordFsChunk> &chunks,
                               uint64_t chunk_size, metadata::ReclaimWork *out);
// Freeze mechanism-private cleanup identities from the authoritative typed
// FileMetadata map without consulting a legacy descriptor or a COW head.
utils::Status FreezeCOWDetachedReclaim(metadata::InodeID ino, const std::vector<metadata::ChunkMapping> &mappings,
                                       metadata::ReclaimWork *out);
utils::Status FreezeCOWDetachedDelete(metadata::InodeID ino, const metadata::ChunkMapping &mapping,
                                      metadata::PendingDelete *out);
utils::Status DecodeCOWDelete(const metadata::PendingDelete &work, uint64_t chunk_size, COWRef *out);
utils::Status DecodeCOWReclaim(const metadata::ReclaimWork &work, uint64_t chunk_size, std::vector<COWRef> *out);

utils::Status CreateCOWCleanupParticipant(uint64_t chunk_size, metadata::ChunkMetadataPtr chunk_metadata,
                                          metadata::IMetaEngine *meta, storage::IDataEngine *data,
                                          internal::ChunkReachabilityProbeFn reachability_probe,
                                          std::unique_ptr<internal::ChunkCleanupParticipant> *out);

}  // namespace swordfs::chunk::cow
