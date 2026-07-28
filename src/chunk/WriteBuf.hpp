// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// WriteBuf — per-file-handle write buffer.  Accumulates writes in memory
// and flushes them to the data engine in chunk-sized units.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/Types.hpp"

namespace swordfs::chunk {

/// Per-open-file-handle write buffer.
///
/// Accumulates incoming writes in a contiguous `std::vector<char>`.
/// When the buffer reaches `max_chunk_size` bytes the caller is
/// expected to flush exactly one chunk's worth of data to the data
/// engine and then call `CommitFlush()` to remove those bytes.
///
/// For force-flush (close / fsync) the caller flushes all remaining
/// data regardless of the threshold.
class WriteBuf {
 public:
  WriteBuf() = default;

  // ──────────────────────────────────────────────────────────────
  // Initialisation
  // ──────────────────────────────────────────────────────────────

  /// Initialise (or re-initialise) the buffer for a given inode.
  void Init(metadata::InodeID ino, size_t max_chunk_size);

  /// True once `Init()` has been called at least once.
  bool IsInit() const { return ino_ != 0; }

  // ──────────────────────────────────────────────────────────────
  // Write path
  // ──────────────────────────────────────────────────────────────

  /// Append |size| bytes from |data|, recording |write_offset| so
  /// that the file size can be updated after flush.
  void Append(const char* data, size_t size, off_t write_offset);

  /// True when the buffer holds at least one full chunk.
  bool ShouldFlush() const { return data_.size() >= max_chunk_size_; }

  /// Return a view of the bytes that should be flushed.
  ///
  /// When |force| is false: returns exactly `max_chunk_size_` bytes
  /// (caller must ensure `ShouldFlush()` is true first).
  /// When |force| is true: returns all buffered data regardless of
  /// size.  Returns an empty view if the buffer is empty.
  std::string_view FlushData(bool force) const;

  /// Remove the first |flushed_size| bytes from the buffer and
  /// advance `next_chunk_`.  Must only be called after the
  /// corresponding `FlushData()` bytes have been persisted.
  void CommitFlush(size_t flushed_size);

  // ──────────────────────────────────────────────────────────────
  // Read-through support
  // ──────────────────────────────────────────────────────────────

  /// The file offset range that the buffered data covers.
  /// Buffer data occupies [BufStart(), BufEnd()).
  off_t BufStart() const;
  off_t BufEnd() const;

  /// Copy up to |size| bytes starting at |off| (relative to
  /// `BufStart()`) into |out|.  Returns the number of bytes copied.
  size_t CopyOut(off_t off, size_t size, std::string* out) const;

  // ──────────────────────────────────────────────────────────────
  // Accessors
  // ──────────────────────────────────────────────────────────────

  metadata::InodeID ino() const { return ino_; }
  uint32_t next_chunk() const { return next_chunk_; }
  size_t max_chunk_size() const { return max_chunk_size_; }
  off_t max_write_end() const { return max_write_end_; }
  size_t size() const { return data_.size(); }
  bool empty() const { return data_.empty(); }

  /// Reset all state — used when re-initialising or clearing.
  void Reset();

 private:
  std::vector<char> data_;
  size_t max_chunk_size_ = 64 * 1024 * 1024;  // 64 MiB default
  metadata::InodeID ino_ = 0;
  uint32_t next_chunk_ = 0;
  off_t max_write_end_ = 0;  // highest byte offset written so far
};

}  // namespace swordfs::chunk
