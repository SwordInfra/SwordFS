// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "chunk/Chunk.hpp"
#include "chunk/cow/WriteBuf.hpp"
#include "metadata/types/Chunk.hpp"
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

  COWChunk(metadata::InodeID ino, metadata::ChunkIndex index, size_t max_chunk_size, metadata::IMetaEngine *meta,
           storage::IDataEngine *data, std::optional<metadata::SwordFsChunk> published_chunk);

  utils::Status Read(size_t offset, size_t len, folly::IOBuf *out) const override;
  utils::Status Write(size_t offset, const folly::IOBuf &data) override;
  utils::Status Flush() override;
  void TruncateLocal(size_t size) override;
  bool HasPendingWrites() const override;

 private:
  metadata::SwordFsChunk BuildMeta(metadata::ChunkRevision revision, size_t size) const;
  utils::Status LoadPublicationBaseline(std::optional<metadata::SwordFsChunk> *out) const;
  utils::Status HydrateForWrite(const metadata::SwordFsChunk &published, std::shared_ptr<WriteBuf> *out) const;
  utils::Status ReadLocal(const WriteBuf &buffer, size_t offset, size_t len, folly::IOBuf *out) const;

 private:
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  mutable utils::FiberRWMutex mutex_;
  std::shared_ptr<WriteBuf> wb_;
  std::shared_ptr<WriteBuf> flushing_wb_;
  State state_ = State::kDirty;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  std::optional<metadata::SwordFsChunk> published_chunk_;
  bool refresh_publication_baseline_ = false;
};

}  // namespace swordfs::chunk::cow
