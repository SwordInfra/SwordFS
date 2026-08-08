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
}  // namespace swordfs

namespace swordfs::chunk {

/// Build the storage key for a chunk: "<inode>/<chunk_index>".
inline std::string MakeChunkKey(metadata::InodeID ino,
                                metadata::ChunkIndex chunk_index) {
  return std::to_string(ino) + "/" + std::to_string(chunk_index);
}

class Chunk {
 public:
  enum class State : uint8_t {
    kWriting,  // accepting writes
    kSealed,   // awaiting / undergoing upload
  };

  /// Create a chunk ready to accept writes.
  Chunk(metadata::InodeID ino, metadata::ChunkIndex index,
        size_t max_chunk_size, storage::IDataEngine *data)
      : ino_(ino), max_chunk_size_(max_chunk_size), wb_(max_chunk_size), index_(index), data_(data) {}

  // ──────────────────────────────────────────────────────────────
  // Write path (only valid while kWriting)
  // ──────────────────────────────────────────────────────────────

  /// Write |size| bytes from |data| at the given chunk-relative offset.
  /// Returns InvalidArgument if the write would exceed chunk bounds.
  utils::Status Write(off_t write_offset, const folly::IOBuf& data);

  /// Return all buffered data for upload.
  std::string_view FlushData() const;

  /// Seal the chunk — no more writes accepted.
  void Seal();

  /// Seal (if writing) and upload to the storage engine.
  /// Returns OK if there is nothing to flush.
  utils::Status Flush();

  // ──────────────────────────────────────────────────────────────
  // Read-through support
  // ──────────────────────────────────────────────────────────────

  /// Read up to |len| bytes starting at chunk-relative |off| into |out|.
  /// The number of bytes copied is available via out->length() increase.
  utils::Status Read(off_t off, size_t len, folly::IOBuf *out) const;

  // ──────────────────────────────────────────────────────────────
  // Accessors
  // ──────────────────────────────────────────────────────────────

  metadata::ChunkIndex index() const { return index_; }
  State state() const { return state_; }
  bool IsWriting() const { return state_ == State::kWriting; }
  bool IsSealed() const { return state_ == State::kSealed; }

  metadata::InodeID ino() const { return ino_; }
  size_t max_chunk_size() const { return max_chunk_size_; }
  size_t size() const { return wb_.size(); }
  bool empty() const { return size() == 0; }

  /// File-offset range: [StartOffset(), EndOffset()).
  off_t StartOffset() const {
    return static_cast<off_t>(index_) *
           static_cast<off_t>(max_chunk_size());
  }
  off_t EndOffset() const { return StartOffset() + static_cast<off_t>(size()); }

  std::string ChunkKey() const { return MakeChunkKey(ino_, index_); }

 private:
  WriteBuf wb_;
  State state_ = State::kWriting;
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  metadata::ChunkIndex index_;
  storage::IDataEngine *data_;
};

}  // namespace swordfs::chunk
