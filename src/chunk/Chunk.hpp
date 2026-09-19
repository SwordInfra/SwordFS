// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Chunk — one logical chunk of a file. Persisted object revisions are
// immutable; rewriting a flushed chunk hydrates its current contents into a
// WriteBuf and publishes a new object revision.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

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

  /// Read exactly |len| bytes starting at chunk-relative |off| into |out|.
  /// A zero-length read is a no-op. On failure, leaves |out| unchanged.
  utils::Status Read(off_t off, size_t len, folly::IOBuf *out) const;

  /// Seal the chunk — no more writes accepted.
  void Seal();

  /// Seal (if writing) and upload to the storage engine.
  /// Returns OK if there is nothing to flush.
  utils::Status Flush();

  /// Discard bytes at or beyond |size| within this chunk while preserving
  /// the surviving prefix for a later flush/read.
  void Truncate(size_t size);

  bool IsFlushed() const {
    return state_ == State::kFlushed;
  }
  bool Flushable() const {
    return (state_ == State::kWriting || state_ == State::kSealed) && wb_ && wb_->size() > 0;
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
      return StartOffset() + static_cast<off_t>(PublishedChunk().size);
    }
    return StartOffset() + static_cast<off_t>(wb_->size());
  }

 private:
  bool IsWriting() const {
    return state_ == State::kWriting;
  }

  /// Build a SwordFsChunk snapshot for metadata registration.
  metadata::SwordFsChunk BuildMeta() const;

  const metadata::SwordFsChunk &PublishedChunk() const;
  utils::Status EnsurePendingRevision();
  utils::Status HydrateForWrite();
  void CompletePublication(const metadata::SwordFsChunk &chunk);

 private:
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  std::unique_ptr<WriteBuf> wb_;
  State state_ = State::kWriting;
  metadata::ChunkIndex index_;
  storage::IDataEngine *data_;
  metadata::IMetaEngine *meta_;
  std::optional<metadata::SwordFsChunk> published_chunk_;
  metadata::ChunkRevision pending_revision_ = metadata::kInvalidChunkRevision;
  // True once the current pending revision has completed at least one
  // successful Put. It lets retry-side conflict resolution ask metadata to
  // classify an already-uploaded candidate without ever treating a revision
  // whose object was never known durable as publishable cleanup work.
  bool pending_object_uploaded_ = false;
};

}  // namespace swordfs::chunk
