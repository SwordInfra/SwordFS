// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemCOWChunkMetadata.hpp"

#include <mutex>

#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

utils::Status MemCOWChunkMetadata::AllocateChunkID(ChunkID *out) {
  utils::ExpectInFiberDomain();
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  return internal::AllocateChunkIDValue(&next_chunk_id_, out);
}

}  // namespace swordfs::metadata
