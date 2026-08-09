// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Chunk — one immutable chunk of a file.  While being written the chunk
// holds a WriteBuf; once full it is sealed and uploaded to the data engine.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "chunk/WriteBuf.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs {
namespace storage {
class IDataEngine;
}
} // namespace swordfs

namespace swordfs::chunk {

class Chunk {
public:
  enum class State : uint8_t {
    kWriting, // accepting writes
    kSealed,  // no more writes, not yet in storage
    kFlushed, // data successfully persisted to storage
  };

  /// Create a chunk ready to accept writes.
  Chunk(metadata::InodeID ino, metadata::ChunkIndex index);

  /// Write |size| bytes from |data| at the given chunk-relative offset.
  /// Returns InvalidArgument if the write would exceed chunk bounds.
  utils::Status Write(off_t write_offset, const folly::IOBuf &data);

  /// Seal the chunk — no more writes accepted.
  void Seal();

  /// Seal (if writing) and upload to the storage engine.
  /// Returns OK if there is nothing to flush.
  utils::Status Flush();

  /// Read up to |len| bytes starting at chunk-relative |off| into |out|.
  /// The number of bytes copied is available via out->length() increase.
  utils::Status Read(off_t off, size_t len, folly::IOBuf *out) const;

  /// Build a ChunkMeta snapshot for metadata registration.
  metadata::ChunkMeta BuildMeta() const;

  bool IsFlushed() const { return state_ == State::kFlushed; }
  bool Flushable() const { return state_ == State::kWriting && wb_.size() > 0; }

  // ──────────────────────────────────────────────────────────────
  // Accessors
  // ──────────────────────────────────────────────────────────────

  metadata::ChunkIndex index() const { return index_; }

  /// File-offset range: [StartOffset(), EndOffset()).
  off_t StartOffset() const {
    return static_cast<off_t>(index_) * static_cast<off_t>(max_chunk_size_);
  }
  off_t EndOffset() const {
    return StartOffset() + static_cast<off_t>(wb_.size());
  }

private:
  bool IsWriting() const { return state_ == State::kWriting; }

  std::string ChunkKey() const {
    return std::to_string(ino_) + "/" + std::to_string(index_);
  }

private:
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  WriteBuf wb_;
  State state_;
  metadata::ChunkIndex index_;
  storage::IDataEngine *data_;
};

} // namespace swordfs::chunk
