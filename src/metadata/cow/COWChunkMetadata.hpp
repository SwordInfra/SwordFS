// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include "metadata/ChunkMetadata.hpp"

namespace swordfs::metadata::cow {

// Typed root for COW-owned metadata. #316 extends this interface with
// ChunkID-keyed COW head/revision operations; it deliberately exposes no raw
// backend fields or FileMetadata transaction hook.
class COWChunkMetadata : public ChunkMetadata {
 public:
  ChunkType Type() const final {
    return ChunkType::kCow;
  }
};

}  // namespace swordfs::metadata::cow
