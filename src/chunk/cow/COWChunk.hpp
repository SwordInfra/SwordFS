// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "chunk/Chunk.hpp"
#include "chunk/cow/WriteBuf.hpp"
#include "metadata/cow/COWChunkMetadata.hpp"
#include "metadata/types/Common.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {
class IMetaEngine;
}

namespace swordfs::storage {
class IDataEngine;
}

namespace swordfs::chunk::cow {

// COW implementation of one logical runtime chunk. The common Chunk
// API deliberately exposes none of this publication/runtime state machine.
class COWChunk final : public Chunk {
 public:
  enum class State : uint8_t {
    kDirty,
    kFlushing,
    kClean,
  };

  COWChunk(metadata::InodeID ino, metadata::ChunkIndex index, size_t max_chunk_size,
           metadata::cow::COWChunkMetadataPtr cow_metadata, metadata::IMetaEngine *meta, storage::IDataEngine *data,
           std::optional<metadata::ChunkID> attached_id,
           std::optional<metadata::cow::COWChunkHead> published_head = std::nullopt, size_t visible_prefix = 0);

  utils::Status Read(size_t offset, size_t len, folly::IOBuf *out) const override;
  utils::Status Write(size_t offset, const folly::IOBuf &data) override;
  utils::Status Flush() override;
  void TruncateLocal(size_t size) override;
  bool HasPendingWrites() const override;

 private:
  utils::Status HydrateForWrite(metadata::ChunkID id, const metadata::cow::COWChunkHead &head,
                                std::shared_ptr<WriteBuf> *out) const;
  utils::Status ReadLocal(const WriteBuf &buffer, size_t offset, size_t len, folly::IOBuf *out) const;

 private:
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  mutable utils::FiberRWMutex mutex_;
  std::shared_ptr<WriteBuf> wb_;
  std::shared_ptr<WriteBuf> flushing_wb_;
  State state_ = State::kDirty;
  metadata::cow::COWChunkMetadataPtr cow_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  std::optional<metadata::ChunkID> attached_id_;
  std::optional<metadata::cow::COWChunkHead> published_head_;
  size_t visible_prefix_ = 0;
};

}  // namespace swordfs::chunk::cow
