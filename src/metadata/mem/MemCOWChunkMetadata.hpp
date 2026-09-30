// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>

#include "metadata/cow/COWChunkMetadata.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {

// Memory implementation of the independent COW ChunkMetadata domain. Its
// identity allocator is synchronized independently from MemMetaStore
// transactions.
class MemCOWChunkMetadata final : public cow::COWChunkMetadata {
 public:
  utils::Status AllocateChunkID(ChunkID *out) override;

 private:
  utils::FiberMutex mutex_;
  uint64_t next_chunk_id_ = 0;
};

}  // namespace swordfs::metadata
