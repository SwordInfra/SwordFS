// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/ChunkFactory.hpp"

#include <memory>

#include "chunk/cow/COWChunk.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/cow/COWChunkMetadata.hpp"

namespace swordfs::chunk {

ChunkFactory::ChunkFactory(metadata::ChunkType chunk_type, metadata::ChunkMetadataPtr chunk_metadata,
                           metadata::IMetaEngine *meta, storage::IDataEngine *data, size_t chunk_size)
    : chunk_type_(chunk_type),
      chunk_metadata_(std::move(chunk_metadata)),
      cow_metadata_(std::dynamic_pointer_cast<metadata::cow::COWChunkMetadata>(chunk_metadata_)),
      meta_(meta),
      data_(data),
      chunk_size_(chunk_size) {
}

utils::Status ChunkFactory::Open(metadata::InodeID ino, metadata::ChunkIndex index, bool create_if_missing,
                                 std::shared_ptr<Chunk> *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("ChunkFactory::Open output is null");
  }
  out->reset();
  if (meta_ == nullptr || data_ == nullptr || chunk_metadata_ == nullptr || chunk_size_ == 0) {
    return utils::Status::Internal("ChunkFactory is not fully initialized");
  }
  if (chunk_metadata_->Type() != chunk_type_) {
    return utils::Status::Internal("ChunkFactory chunk metadata type mismatch");
  }
  if (chunk_type_ != metadata::ChunkType::kCow) {
    return utils::Status::NotSupported("selected chunk type has no runtime implementation");
  }
  if (cow_metadata_ == nullptr) {
    return utils::Status::Internal("ChunkFactory COW metadata capability mismatch");
  }

  metadata::SwordFsChunk published;
  auto status = meta_->FindChunk(ino, index, &published);
  if (status.ok()) {
    if (published.index != index || !published.IsValidForChunkSize(chunk_size_)) {
      return utils::Status::Malformed("ChunkFactory::Open loaded an invalid chunk descriptor");
    }
    *out = std::make_shared<cow::COWChunk>(ino, index, chunk_size_, cow_metadata_, meta_, data_, published);
    return utils::Status::OK();
  }
  if (!status.IsNotFound()) {
    return status;
  }
  if (!create_if_missing) {
    return utils::Status::OK();
  }
  *out = std::make_shared<cow::COWChunk>(ino, index, chunk_size_, cow_metadata_, meta_, data_, std::nullopt);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
