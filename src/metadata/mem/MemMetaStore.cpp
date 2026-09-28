// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaStore.hpp"

#include "chunk/IChunkOverwriteStrategy.hpp"
#include "metadata/mem/MemPrivateMetadataStore.hpp"

namespace swordfs::metadata {

utils::Status MemMetaStore::BindChunkOverwriteStrategy(chunk::IChunkOverwriteStrategy *strategy) {
  if (strategy == nullptr) {
    return utils::Status::InvalidArgument("chunk strategy is null");
  }

  auto private_metadata = std::make_shared<MemPrivateMetadataStore>(strategy->mechanism());
  auto status = strategy->BindPrivateMetadata(private_metadata);
  if (!status.ok()) {
    return status;
  }
  chunk_strategy_ = strategy;
  private_metadata_ = std::move(private_metadata);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
