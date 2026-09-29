// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/ChunkFactory.hpp"

#include <memory>

#include "chunk/WholeObjectChunk.hpp"
#include "metadata/IMetaEngine.hpp"

namespace swordfs::chunk {

utils::Status ChunkFactory::Open(metadata::InodeID ino, metadata::ChunkIndex index, bool create_if_missing,
                                 std::shared_ptr<Chunk> *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("ChunkFactory::Open output is null");
  }
  out->reset();
  if (meta_ == nullptr || data_ == nullptr || private_metadata_ == nullptr || chunk_size_ == 0) {
    return utils::Status::Internal("ChunkFactory is not fully initialized");
  }
  if (private_metadata_->mechanism() != mechanism_) {
    return utils::Status::Internal("ChunkFactory private metadata mechanism mismatch");
  }
  if (mechanism_ != metadata::ChunkOverwriteMechanism::kWholeObject) {
    return utils::Status::NotSupported("selected chunk mechanism has no runtime implementation");
  }

  metadata::SwordFsChunk published;
  auto status = meta_->FindChunk(ino, index, &published);
  if (status.ok()) {
    if (published.index != index || !published.IsValidForChunkSize(chunk_size_)) {
      return utils::Status::Malformed("ChunkFactory::Open loaded an invalid chunk descriptor");
    }
    *out = std::make_shared<WholeObjectChunk>(ino, index, chunk_size_, meta_, data_, published);
    return utils::Status::OK();
  }
  if (!status.IsNotFound()) {
    return status;
  }
  if (!create_if_missing) {
    return utils::Status::OK();
  }
  *out = std::make_shared<WholeObjectChunk>(ino, index, chunk_size_, meta_, data_, std::nullopt);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
