// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Slice — immutable data fragment within a chunk.
//
// In the Slice model, each Write creates one or more new Slices that
// are appended to the chunk's slice list.  Slices are immutable once
// flushed to S3; random overwrites produce new slices that overlap or
// supersede older ones.  Reads stitch the visible slices together to
// reconstruct the current file content.
//
// Chunk keys in MemMetaStore use the format "A{ino}C{chunk_index}"
// (A = inode, C = chunk) and map to a serialized SliceList.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swordfs::storage {

/// A single immutable data fragment within a chunk.
struct Slice {
  uint64_t id = 0;      // opaque slice id, doubles as S3 object key suffix
  uint32_t offset = 0;  // byte offset within the chunk
  uint32_t length = 0;  // payload length in bytes (0 = padding / removed)

  /// Comparison for sorting by chunk offset.
  bool operator<(const Slice& other) const { return offset < other.offset; }
};

/// Ordered list of slices that make up a chunk.
///
/// Serialized to/from a byte string for storage in MemMetaStore.
struct SliceList {
  uint64_t next_slice_id = 1;  // counter for allocating new slice ids
  std::vector<Slice> slices;

  /// Allocate the next slice id and advance the counter.
  uint64_t AllocID() { return next_slice_id++; }

  /// Serialize to a compact binary format.
  //   [next_slice_id:8][count:4][slice...]
  //   each slice: [id:8][offset:4][length:4]
  std::string Serialize() const;

  /// Deserialize from bytes produced by Serialize().
  /// Returns true on success.
  bool Deserialize(std::string_view data);

  bool empty() const { return slices.empty(); }
  size_t size() const { return slices.size(); }
};

/// Build a MemMetaStore chunk key from inode + chunk index.
inline std::string ChunkMetaKey(uint64_t ino, uint64_t chunk_idx) {
  return "A" + std::to_string(ino) + "C" + std::to_string(chunk_idx);
}

}  // namespace swordfs::storage
