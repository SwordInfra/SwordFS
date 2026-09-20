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
    kDirty,     // latest complete local data is not yet confirmed authoritative
    kFlushing,  // transient publication attempt under the file operation lock
    kClean,     // authoritative publication is explicitly confirmed
  };

  /// Create a chunk ready to accept writes.
  Chunk(metadata::InodeID ino, metadata::ChunkIndex index);

  /// Query VolumeImpl's meta engine for existing published metadata at
  /// this chunk's start offset. If found, transition to kClean; otherwise
  /// stay in kDirty so the caller can write into the local buffer.
  utils::Status Initialize();

  /// Write |size| bytes from |data| at the given chunk-relative offset.
  /// Returns InvalidArgument if the write would exceed chunk bounds.
  utils::Status Write(off_t write_offset, const folly::IOBuf &data);

  /// Read exactly |len| bytes starting at chunk-relative |off| into |out|.
  /// A zero-length read is a no-op. On failure, leaves |out| unchanged.
  utils::Status Read(off_t off, size_t len, folly::IOBuf *out) const;

  /// Publish the latest dirty buffer. A publication attempt is transient:
  /// every non-successful outcome returns the chunk to kDirty with the local
  /// data retained and writable.
  utils::Status Flush();

  /// Discard bytes at or beyond |size| within this chunk while preserving
  /// the surviving prefix for a later flush/read.
  void Truncate(size_t size);

  bool IsClean() const {
    return state_ == State::kClean;
  }
  bool Flushable() const {
    return state_ == State::kDirty && wb_->size() > 0;
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
    if (IsClean()) {
      return StartOffset() + static_cast<off_t>(PublishedChunk().size);
    }
    return StartOffset() + static_cast<off_t>(wb_->size());
  }

 private:
  /// Build a SwordFsChunk snapshot for metadata registration using |revision|.
  metadata::SwordFsChunk BuildMeta(metadata::ChunkRevision revision) const;

  const metadata::SwordFsChunk &PublishedChunk() const;
  utils::Status RefreshPublicationBaseline();
  utils::Status HydrateForWrite();
  void CompletePublication(const metadata::SwordFsChunk &chunk);

 private:
  metadata::InodeID ino_;
  size_t max_chunk_size_;
  std::unique_ptr<WriteBuf> wb_;
  State state_ = State::kDirty;
  metadata::ChunkIndex index_;
  storage::IDataEngine *data_;
  metadata::IMetaEngine *meta_;
  std::optional<metadata::SwordFsChunk> published_chunk_;
  bool refresh_publication_baseline_ = false;
};

}  // namespace swordfs::chunk
