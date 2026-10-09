// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/ChunkFactory.hpp"

#include <algorithm>
#include <memory>

#include "chunk/cow/COWChunk.hpp"
#include "chunk/cow/COWChunkBoundary.hpp"
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

utils::Status ChunkFactory::SanitizeBoundary(metadata::InodeID ino,
                                             const metadata::RetainedChunkBoundary &boundary) const {
  if (cow_metadata_ == nullptr || meta_ == nullptr) {
    return utils::Status::NotSupported("selected chunk type cannot sanitize a retained boundary");
  }
  return cow::SanitizeCOWBoundary(*cow_metadata_, boundary,
                                  [&](metadata::ChunkIndex index, std::optional<metadata::ChunkID> *attached) {
                                    return meta_->ProbeAttachment(ino, index, attached);
                                  });
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

  metadata::FileChunkSnapshot file;
  auto status = meta_->ReadFileChunkSnapshot(ino, index, &file);
  if (!status.ok()) {
    return status;
  }
  if (file.chunk_id.has_value()) {
    metadata::cow::COWChunkHead head;
    status = cow_metadata_->GetHead(*file.chunk_id, &head);
    if (status.IsNotFound()) {
      return utils::Status::Malformed("ChunkFactory::Open: mapped ChunkID is missing its private COW head");
    }
    if (!status.ok()) {
      return status;
    }
    if (!metadata::cow::internal::IsValidHead(head) || head.size > chunk_size_) {
      return utils::Status::Malformed("ChunkFactory::Open: invalid private COW head");
    }
    uint64_t start = 0;
    status = metadata::CalculateChunkStartOffset(index, chunk_size_, &start);
    if (!status.ok()) {
      return status;
    }
    const uint64_t visible =
        file.inode.attr.size > start ? std::min<uint64_t>(head.size, file.inode.attr.size - start) : 0;
    *out = std::make_shared<cow::COWChunk>(ino, index, chunk_size_, cow_metadata_, meta_, data_, file.chunk_id, head,
                                           static_cast<size_t>(visible));
    return utils::Status::OK();
  }
  if (!create_if_missing) {
    return utils::Status::OK();
  }
  *out = std::make_shared<cow::COWChunk>(ino, index, chunk_size_, cow_metadata_, meta_, data_, std::nullopt);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
