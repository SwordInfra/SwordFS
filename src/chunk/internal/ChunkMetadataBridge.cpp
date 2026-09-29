// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkMetadataBridge.hpp"

#include <memory>
#include <string>
#include <utility>

#include "chunk/cow/COWChunkMetadataBridge.hpp"

namespace swordfs::chunk::internal {

utils::Status CreateChunkMetadataBridge(metadata::ChunkType chunk_type,
                                        metadata::MechanismPrivateStorePtr private_metadata,
                                        std::unique_ptr<ChunkMetadataBridge> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk metadata bridge output is null");
  }
  out->reset();
  if (private_metadata == nullptr || private_metadata->mechanism() != chunk_type) {
    return utils::Status::InvalidArgument("chunk metadata bridge private-store chunk type mismatch");
  }
  if (chunk_type == metadata::ChunkType::kCow) {
    return cow::CreateCOWChunkMetadataBridge(std::move(private_metadata), out);
  }
  const auto name = metadata::ChunkTypeName(chunk_type);
  return utils::Status::NotSupported("unsupported chunk metadata bridge type: " + std::string(name));
}

}  // namespace swordfs::chunk::internal
