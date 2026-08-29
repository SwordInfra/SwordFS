// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Chunk — one immutable chunk of a file.  While being written the chunk
// holds a WriteBuf; once full it is sealed and uploaded to the data engine.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "chunk/WriteBuf.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs {
namespace metadata {
class IMetaEngine;
}
namespace storage {
class IDataEngine;
}
}  // namespace swordfs

namespace swordfs::chunk {

inline std::string FormatChunkKey(metadata::InodeID ino, metadata::ChunkIndex idx) {
  return std::to_string(ino) + "/" + std::to_string(idx);
}

class Chunk {
 public:
  enum class State : uint8_t {
    kWriting,  // accepting writes
    kSealed,   // no more writes, not yet in storage
    kFlushed,  // data successfully persisted to storage
  };

  /// Create a chunk ready to accept writes.
  Chunk(metadata::InodeID ino, metadata::ChunkIndex index);

  /// Query VolumeImpl's meta engine for existing flushed metadata at
  /// this chunk's start offset.  If found, transition to kFlushed;
  /// otherwise stay in kWriting so the caller can write into it.
  utils::Status Initialize();

  /// Write |size| bytes from |data| at the given chunk-relative offset.
  /// Returns InvalidArgument if the write would exceed chunk bounds.
  utils::Status Write(off_t write_offset, const folly::IOBuf &data);

  /// Read up to |len| bytes starting at chunk-relative |off| into |out|.
  /// The number of bytes copied is available via out->length() increase.
  utils::Status Read(off_t off, size_t len, folly::IOBuf *out) const;

  /// Seal the chunk — no more writes accepted.
  void Seal();

  /// Seal (if writing) and upload to the storage engine.
  /// Returns OK if there is nothing to flush.
  utils::Status Flush();

  bool IsFlushed() const {
    return state_ == State::kFlushed;
  }
  bool Flushable() const {
    return state_ == State::kWriting && wb_ && wb_->size() > 0;
  }

  // ──────────────────────────────────────────────────────────────
  // Accessors
  // ──────────────────────────────────────────────────────────────

  metadata::ChunkIndex index() const {
    return index_;
  }

  /// File-offset range: [StartOffset(), EndOffset()).
  off_t StartOffset() const {
    return static_cast<off_t>(index_) * static_cast<off_t>(max_chunk_size_);
  }
  off_t DataEnd() const {
    if (IsFlushed()) {
      return StartOffset() + static_cast<off_t>(flushed_size_);
    }
    return StartOffset() + static_cast<off_t>(wb_ ? wb_->size() : 0);
  }

 private:
  bool IsWriting() const {
    return state_ == State::kWriting;
  }

  /// Build a SwordFsChunk snapshot for metadata registration.
  metadata::SwordFsChunk BuildMeta() const;

  std::string ChunkKey() const {
    return FormatChunkKey(ino_, index_);
  }

 private:
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  std::unique_ptr<WriteBuf> wb_;
  State state_;
  metadata::ChunkIndex index_;
  storage::IDataEngine *data_;
  metadata::IMetaEngine *meta_;
  size_t flushed_size_ = 0;  // valid only when kFlushed
};

}  // namespace swordfs::chunk
