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

namespace swordfs::chunk {

class Chunk {
 public:
  enum class State : uint8_t {
    kWriting,  // accepting writes
    kSealed,   // awaiting / undergoing upload
  };

  /// Create a chunk ready to accept writes.
  Chunk(uint32_t index, metadata::InodeID ino, size_t max_chunk_size)
      : ino_(ino), max_chunk_size_(max_chunk_size), wb_(max_chunk_size), index_(index) {}

  // ──────────────────────────────────────────────────────────────
  // Write path (only valid while kWriting)
  // ──────────────────────────────────────────────────────────────

  /// Write |size| bytes from |data| at the given chunk-relative offset.
  /// Returns InvalidArgument if the write would exceed chunk bounds.
  utils::Status Write(const char* data, size_t size, off_t write_offset);

  /// Return all buffered data for upload.
  std::string_view FlushData() const;

  /// Seal the chunk — no more writes accepted.
  void Seal();

  // ──────────────────────────────────────────────────────────────
  // Read-through support
  // ──────────────────────────────────────────────────────────────

  /// Copy up to |len| bytes starting at |off| (relative to chunk
  /// start) into |out|.  Returns the number of bytes copied.
  utils::Status CopyOut(off_t off, size_t len, folly::IOBuf* out) const;

  // ──────────────────────────────────────────────────────────────
  // Accessors
  // ──────────────────────────────────────────────────────────────

  uint32_t index() const { return index_; }
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

  std::string ChunkKey() const {
    return std::to_string(ino_) + "/" + std::to_string(index_);
  }

 private:
  WriteBuf wb_;
  State state_ = State::kWriting;
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  uint32_t index_;
};

}  // namespace swordfs::chunk
