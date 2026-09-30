// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileReadWriter — per-inode read/write over a chain of chunks.
// Shared across all file handles that reference the same inode so that
// writes from one handle are visible to reads on another (before flush).
//
// Writes are buffered in a per-inode dirty deque; Flush seals, uploads,
// and registers chunk metadata with the metadata engine so that
// subsequent reads can locate the data via storage.

#pragma once

#include <folly/container/F14Map.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "chunk/Chunk.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs {

namespace chunk {
class ChunkFactory;
}

namespace metadata {
class IMetaEngine;
}

namespace vfs {

class InodeHandle;
class LiveAttrGuard;

// ────────────────────────────────────────────────────────────────
// FileChunkManager — thread-safe manager of inode chunks (both dirty and
// flushed).  All synchronisation is internal.  Flushed chunks are
// not removed so reads can reach them directly via Chunk::Read().
// ────────────────────────────────────────────────────────────────

class FileChunkManager {
 public:
  using Map = folly::F14FastMap<metadata::ChunkIndex, std::shared_ptr<chunk::Chunk>>;

  FileChunkManager(metadata::InodeID ino, const chunk::ChunkFactory *factory) : ino_(ino), factory_(factory) {
  }

  /// Get the chunk at |idx|. If not in the map, creates and initializes it.
  /// A successful lookup with |*out == nullptr| means no materialized chunk
  /// exists and |create_if_missing| is false. Initialization failures are
  /// returned as Status and are never encoded as a null chunk.
  /// The shared pointer keeps the chunk alive if the map is changed.
  utils::Status Get(metadata::ChunkIndex idx, bool create_if_missing, std::shared_ptr<chunk::Chunk> *out);

  /// Snapshot chunks with pending data, including sealed chunks from a
  /// previous failed flush. The caller may attempt each snapshot entry once
  /// without repeatedly selecting the same failed chunk.
  std::vector<std::shared_ptr<chunk::Chunk>> GetPendingWrites();

  /// Apply a file-size change to cached chunks. A partial boundary chunk keeps
  /// only its surviving prefix; chunks wholly beyond EOF are dropped locally.
  /// Authoritative metadata owns best-effort object cleanup registration.
  void TruncateToSize(size_t size, size_t chunk_size);

 private:
  metadata::InodeID ino_;
  const chunk::ChunkFactory *factory_;
  mutable utils::FiberMutex mutex_;
  Map chunks_;
};

class FileReadWriter {
 public:
  using InodeID = metadata::InodeID;

  FileReadWriter(InodeID ino, size_t max_parallel_flushes);

  /// Write the contents of |buf| at |off|, splitting across chunk boundaries.
  utils::Status Write(const folly::IOBuf &buf, off_t off, int open_flags = 0);

  /// Read up to |size| bytes at |off| into |out|.
  /// |out| must be an empty IOBuf with capacity >= |size|.
  /// On success, out->length() reflects bytes actually read.
  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  /// Read authoritative inode metadata and compose the transient local file
  /// size while holding the inode operation lock.
  utils::Status GetAttr(metadata::SwordFsInode *out) const;

  /// Seal and upload all dirty chunks, then register them with the
  /// metadata engine.
  utils::Status Flush();

  /// Truncate to |size| bytes.  Updates chunk metadata in the metadata
  /// engine and drops cached chunks.  Used by O_TRUNC (size=0).
  utils::Status Truncate(size_t size);

  /// Apply setattr while coordinating size changes with the local chunk
  /// cache so pending writes cannot be republished past a successful truncate.
  utils::Status SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields, metadata::SwordFsInode *out);

  utils::Status SetInodeFlags(metadata::InodeFlag inode_flags, metadata::SwordFsInode *out);

 private:
  friend class LiveAttrGuard;
  friend class InodeHandle;

  void InitializeCreatedState(uint64_t size);
  uint64_t SnapshotInodeFlagEpoch() const;
  utils::Status ReconcileOpenState(uint64_t size, metadata::InodeFlag inode_flags, uint64_t observed_flag_epoch,
                                   int open_flags);
  utils::Status GetVisibleSize(uint64_t *size);
  void ApplyLiveSize(metadata::SwordFsInode *inode) const;
  utils::Status FlushPendingWritesLocked();

 private:
  InodeID ino_;
  size_t chunk_size_;
  size_t max_parallel_flushes_;
  metadata::IMetaEngine *meta_;
  // Ordinary data-path operations share this lock; size-changing operations
  // take exclusive ownership because they change file-wide reachability.
  mutable utils::FiberRWMutex operation_mutex_;
  utils::FiberMutex flush_mutex_;
  FileChunkManager chunks_;
  // The size state composes the authoritative size captured for this local
  // inode lifetime with a transient lower bound from accepted local writes.
  // Reads use the same composition as attribute replies for their EOF bound.
  mutable utils::FiberMutex size_mutex_;
  mutable std::optional<uint64_t> authoritative_size_;
  std::optional<uint64_t> live_size_;
  std::atomic<metadata::InodeFlag> inode_flags_{metadata::InodeFlag::kNone};
  mutable utils::FiberMutex inode_policy_mutex_;
  // Advances after every successful local flag mutation so an Open metadata
  // snapshot captured before that mutation cannot overwrite newer policy.
  uint64_t inode_flag_epoch_ = 0;
  uint64_t size_state_epoch_ = 0;
  uint64_t write_epoch_ = 0;
};

class LiveAttrGuard {
 public:
  LiveAttrGuard(LiveAttrGuard &&) noexcept = default;
  LiveAttrGuard &operator=(LiveAttrGuard &&) noexcept = default;
  LiveAttrGuard(const LiveAttrGuard &) = delete;
  LiveAttrGuard &operator=(const LiveAttrGuard &) = delete;

  void Apply(metadata::SwordFsInode &inode) const;

 private:
  friend class InodeHandle;

  explicit LiveAttrGuard(std::shared_ptr<FileReadWriter> owner);

  std::shared_ptr<FileReadWriter> owner_;
  std::shared_lock<utils::FiberRWMutex> lock_;
};

}  // namespace vfs
}  // namespace swordfs
