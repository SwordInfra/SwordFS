// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>
#include <optional>

#include "metadata/cow/COWChunkMetadata.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {

// Memory implementation of the independent COW ChunkMetadata domain. Its
// identity allocator is synchronized independently from MemMetaStore
// transactions.
class MemCOWChunkMetadata final : public cow::COWChunkMetadata {
 public:
  utils::Status AllocateChunkID(ChunkID *out) override;
  utils::Status AllocateRevision(ChunkID chunk_id, cow::COWChunkRevision *out) override;
  utils::Status GetHead(ChunkID chunk_id, cow::COWChunkHead *out) override;
  utils::Status CompareExchangeHead(ChunkID chunk_id, const std::optional<cow::COWChunkHead> &expected,
                                    const cow::COWChunkHead &replacement) override;
  utils::Status EraseHead(ChunkID chunk_id, const cow::COWChunkHead &expected) override;

 private:
  utils::FiberMutex mutex_;
  uint64_t next_chunk_id_ = 0;
  folly::F14FastMap<uint64_t, uint64_t> next_revisions_;
  folly::F14FastMap<uint64_t, cow::COWChunkHead> heads_;
};

}  // namespace swordfs::metadata
