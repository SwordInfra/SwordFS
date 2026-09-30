// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "chunk/Chunk.hpp"
#include "metadata/ChunkMetadata.hpp"
#include "metadata/types/Volume.hpp"

namespace swordfs::metadata {
class IMetaEngine;
}

namespace swordfs::storage {
class IDataEngine;
}

namespace swordfs::chunk {

// Mount-owned construction boundary for logical chunks. Mechanism selection
// is fixed once at mount and is never exposed to VFS callers.
class ChunkFactory {
 public:
  ChunkFactory(metadata::ChunkType chunk_type, metadata::ChunkMetadataPtr chunk_metadata, metadata::IMetaEngine *meta,
               storage::IDataEngine *data, size_t chunk_size)
      : chunk_type_(chunk_type),
        chunk_metadata_(std::move(chunk_metadata)),
        meta_(meta),
        data_(data),
        chunk_size_(chunk_size) {
  }

  utils::Status Open(metadata::InodeID ino, metadata::ChunkIndex index, bool create_if_missing,
                     std::shared_ptr<Chunk> *out) const;

 private:
  metadata::ChunkType chunk_type_;
  // #316 consumes this through the typed COW metadata interface. Retain the
  // mount-lifetime capability here so VFS never needs mechanism metadata
  // plumbing.
  metadata::ChunkMetadataPtr chunk_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  size_t chunk_size_;
};

}  // namespace swordfs::chunk
