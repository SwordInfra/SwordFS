// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaStore.hpp"

#include "chunk/internal/ChunkMetadataBridge.hpp"
#include "metadata/mem/MemPrivateMetadataStore.hpp"

namespace swordfs::metadata {

utils::Status MemMetaStore::OpenPrivateMetadataStore(ChunkOverwriteMechanism mechanism, MechanismPrivateStorePtr *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("private metadata store output is null");
  }
  private_metadata_ = std::make_shared<MemPrivateMetadataStore>(mechanism);
  *out = private_metadata_;
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
