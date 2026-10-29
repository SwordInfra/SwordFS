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

namespace swordfs::metadata {
class IMetaEngine;
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
  ~ChunkFactory();

  // Mounted Slice callers execute in the fiber domain; the mechanism-private
  // per-inode publication registry follows that same execution contract.
  utils::Status Open(metadata::InodeID ino, metadata::ChunkIndex index, bool create_if_missing,
                     std::shared_ptr<Chunk> *out) const;

  // The VFS owns size ordering but not the mechanism's private head CAS.
  // Keep boundary sanitation behind the mount-selected chunk factory.
  utils::Status SanitizeBoundary(metadata::InodeID ino, const metadata::RetainedChunkBoundary &boundary) const;

 private:
  // Mechanism-private mount-local coordination, never exposed to VFS callers.
  struct SlicePublicationRegistry;

 private:
  metadata::ChunkType chunk_type_;
  // Retain a typed, mount-lifetime metadata capability for mechanism dispatch.
  metadata::ChunkMetadataPtr chunk_metadata_;
  std::shared_ptr<metadata::slice::SliceChunkMetadata> slice_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  size_t chunk_size_;
  std::unique_ptr<SlicePublicationRegistry> slice_publication_registry_;
};

}  // namespace swordfs::chunk
