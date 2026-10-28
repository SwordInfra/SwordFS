// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "chunk/Chunk.hpp"
#include "metadata/ChunkMetadata.hpp"
#include "metadata/ChunkSizePlan.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {
class IMetaEngine;
namespace cow {
class COWChunkMetadata;
}
namespace slice {
class SliceChunkMetadata;
}
}  // namespace swordfs::metadata

namespace swordfs::storage {
class IDataEngine;
}

namespace swordfs::chunk {

// Mount-owned construction boundary for logical chunks. Mechanism selection
// is fixed once at mount and is never exposed to VFS callers.
class ChunkFactory {
 public:
  ChunkFactory(metadata::ChunkType chunk_type, metadata::ChunkMetadataPtr chunk_metadata, metadata::IMetaEngine *meta,
               storage::IDataEngine *data, size_t chunk_size);

  // Slice requires the inode owner's shared publication gate. The optional
  // argument is unused for COW; omitting it for Slice fails closed.
  utils::Status Open(metadata::InodeID ino, metadata::ChunkIndex index, bool create_if_missing,
                     std::shared_ptr<Chunk> *out, std::shared_ptr<utils::FiberMutex> publication_mutex = nullptr) const;

  // The VFS owns size ordering but not the mechanism's private head CAS.
  // Keep boundary sanitation behind the mount-selected chunk factory.
  utils::Status SanitizeBoundary(metadata::InodeID ino, const metadata::RetainedChunkBoundary &boundary) const;

 private:
  metadata::ChunkType chunk_type_;
  // #316 consumes this through the typed COW metadata interface. Retain the
  // mount-lifetime capability here so VFS never needs mechanism metadata
  // plumbing.
  metadata::ChunkMetadataPtr chunk_metadata_;
  std::shared_ptr<metadata::cow::COWChunkMetadata> cow_metadata_;
  std::shared_ptr<metadata::slice::SliceChunkMetadata> slice_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  size_t chunk_size_;
};

}  // namespace swordfs::chunk
