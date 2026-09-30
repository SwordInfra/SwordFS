// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaStore.hpp"

#include "chunk/internal/ChunkMetadataBridge.hpp"
#include "metadata/mem/MemCOWChunkMetadata.hpp"

namespace swordfs::metadata {

utils::Status MemMetaStore::OpenChunkMetadata(ChunkType chunk_type, ChunkMetadataPtr *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk metadata output is null");
  }
  if (chunk_type != ChunkType::kCow) {
    return utils::Status::NotSupported("chunk metadata type is not implemented: " +
                                       std::string(ChunkTypeName(chunk_type)));
  }
  if (chunk_metadata_ != nullptr) {
    *out = chunk_metadata_;
    return utils::Status::OK();
  }
  chunk_type_ = chunk_type;
  chunk_metadata_ = std::make_shared<MemCOWChunkMetadata>();
  *out = chunk_metadata_;
  return utils::Status::OK();
}

utils::Status MemMetaStore::BindChunkMetadataBridge(chunk::internal::ChunkMetadataBridge *bridge) {
  if (bridge == nullptr) {
    return utils::Status::InvalidArgument("chunk metadata bridge is null");
  }
  chunk_metadata_bridge_ = bridge;
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
