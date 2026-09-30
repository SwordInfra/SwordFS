// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <limits>
#include <memory>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Redis INCR is signed 64-bit. Keeping ChunkID in this range lets Memory and
// Redis expose one portable volume-scoped identity contract.
inline constexpr uint64_t kMaxChunkIDValue = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

namespace internal {

// Checked in-memory transition shared by the Memory implementation and unit
// tests. Production callers allocate through ChunkMetadata.
utils::Status AllocateChunkIDValue(uint64_t *current, ChunkID *out);

}  // namespace internal

// Mount-scoped root for the selected mechanism's metadata domain.
//
// The common surface intentionally contains no raw record or transaction
// operations. Concrete mechanisms add typed APIs keyed by ChunkID while
// retaining independent atomicity from FileMetadata.
class ChunkMetadata {
 public:
  virtual ~ChunkMetadata() = default;

  virtual ChunkType Type() const = 0;
  virtual utils::Status AllocateChunkID(ChunkID *out) = 0;
};

using ChunkMetadataPtr = std::shared_ptr<ChunkMetadata>;

}  // namespace swordfs::metadata
