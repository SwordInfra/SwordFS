// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkMetadataBridge.hpp"

#include <memory>
#include <string>

#include "chunk/cow/COWChunkMetadataBridge.hpp"

namespace swordfs::chunk::internal {

utils::Status CreateChunkMetadataBridge(metadata::ChunkType chunk_type, std::unique_ptr<ChunkMetadataBridge> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk metadata bridge output is null");
  }
  out->reset();
  if (chunk_type == metadata::ChunkType::kCow) {
    return cow::CreateCOWChunkMetadataBridge(out);
  }
  const auto name = metadata::ChunkTypeName(chunk_type);
  return utils::Status::NotSupported("unsupported chunk metadata bridge type: " + std::string(name));
}

}  // namespace swordfs::chunk::internal
