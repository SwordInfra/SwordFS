// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/ChunkFactory.hpp"

#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "chunk/slice/SliceChunk.hpp"
#include "chunk/slice/SliceRecord.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"

namespace swordfs::chunk {

struct ChunkFactory::SlicePublicationRegistry {
  std::shared_ptr<utils::FiberMutex> ForInode(metadata::InodeID ino) {
    std::lock_guard<utils::FiberMutex> lock(mutex_);
    auto &weak = gates_[ino];
    auto gate = weak.lock();
    if (!gate) {
      gate = std::make_shared<utils::FiberMutex>();
      weak = gate;
      // Expired entries must not accumulate indefinitely on a long-running
      // mount as it opens and closes distinct inodes.
      if (++created_since_cleanup_ >= 256) {
        for (auto it = gates_.begin(); it != gates_.end();) {
          it = it->second.expired() ? gates_.erase(it) : std::next(it);
        }
        created_since_cleanup_ = 0;
      }
    }
    return gate;
  }

 private:
  utils::FiberMutex mutex_;
  std::map<metadata::InodeID, std::weak_ptr<utils::FiberMutex>> gates_;
  size_t created_since_cleanup_ = 0;
};

ChunkFactory::ChunkFactory(metadata::ChunkType chunk_type, metadata::ChunkMetadataPtr chunk_metadata,
                           metadata::IMetaEngine *meta, storage::IDataEngine *data, size_t chunk_size)
    : chunk_type_(chunk_type),
      chunk_metadata_(std::move(chunk_metadata)),
      slice_metadata_(std::dynamic_pointer_cast<metadata::slice::SliceChunkMetadata>(chunk_metadata_)),
      meta_(meta),
      data_(data),
      chunk_size_(chunk_size),
      slice_publication_registry_(
          chunk_type == metadata::ChunkType::kChunkSlice ? std::make_unique<SlicePublicationRegistry>() : nullptr) {
}

ChunkFactory::~ChunkFactory() = default;

utils::Status ChunkFactory::SanitizeBoundary(metadata::InodeID ino,
                                             const metadata::RetainedChunkBoundary &boundary) const {
  if (chunk_type_ == metadata::ChunkType::kChunkSlice) {
    if (slice_metadata_ == nullptr || meta_ == nullptr) {
      return utils::Status::NotSupported("selected chunk type cannot sanitize a retained boundary");
    }
    if (boundary.chunk_id == metadata::kInvalidChunkID || boundary.chunk_id.Value() > metadata::kMaxChunkIDValue ||
        boundary.visible_prefix == 0 || boundary.visible_prefix >= chunk_size_ ||
        chunk_size_ > std::numeric_limits<uint32_t>::max()) {
      return utils::Status::InvalidArgument("invalid retained Slice boundary");
    }

    std::optional<metadata::ChunkID> attached;
    auto status = meta_->ProbeAttachment(ino, boundary.index, &attached);
    if (!status.ok()) {
      return status;
    }
    if (attached != boundary.chunk_id) {
      // FileMetadata has already detached/replaced this identity. Never
      // recreate a private index for an obsolete ChunkID.
      return utils::Status::OK();
    }
    // Durable zero-cover, not a physical object: the ordered Slice reader
    // treats SliceID{} as zeros and a later committed Write can overlay it.
    // Cover the entire suffix to prevent old bytes returning after Grow.
    const auto prefix = static_cast<uint32_t>(boundary.visible_prefix);
    status = slice_metadata_->AppendRecord(
        boundary.chunk_id,
        slice::SliceRecord{prefix, slice::SliceID(), 0, 0, static_cast<uint32_t>(chunk_size_ - prefix)});
    if (status.IsNotFound()) {
      // RPUSHX requires a pre-existing index. A missing index still mapped
      // by FileMetadata is corruption, not an implicit hole; a concurrent
      // detach is harmless. Do not recreate a lost private index here.
      status = meta_->ProbeAttachment(ino, boundary.index, &attached);
      if (!status.ok()) {
        return status;
      }
      return attached == boundary.chunk_id ? utils::Status::Malformed("mapped ChunkID is missing its Slice index")
                                           : utils::Status::OK();
    }
    // In particular, OutcomeUnknown must not be interpreted as successful
    // sanitation or followed by a blind duplicate retry.
    return status;
  }
  return utils::Status::NotSupported("selected chunk type cannot sanitize a retained boundary");
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
  if (chunk_type_ == metadata::ChunkType::kChunkSlice) {
    if (slice_metadata_ == nullptr) {
      return utils::Status::NotSupported("selected chunk type has no Slice metadata capability");
    }
    metadata::FileChunkSnapshot file;
    auto status = meta_->ReadFileChunkSnapshot(ino, index, &file);
    if (!status.ok()) {
      return status;
    }
    if (!file.chunk_id.has_value() && !create_if_missing) {
      return utils::Status::OK();
    }
    *out = std::make_shared<slice::SliceChunk>(ino, index, chunk_size_, slice_metadata_, meta_, data_,
                                               slice_publication_registry_->ForInode(ino));
    return utils::Status::OK();
  }
  return utils::Status::NotSupported("selected chunk type has no runtime implementation");
}

}  // namespace swordfs::chunk
