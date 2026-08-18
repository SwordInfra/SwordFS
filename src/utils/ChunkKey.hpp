// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// ChunkKey — canonical object-storage key for a (ino, chunk_index) pair.
//
// Both the VFS layer (when deleting chunk objects on inode reclaim) and the
// `chunk` layer (when uploading or reading a chunk) need to compute the same
// string key for a given chunk. Centralising the formula here keeps the two
// callers in sync.

#pragma once

#include <cstdint>
#include <string>

#include "metadata/Types.hpp"

namespace swordfs::utils {

// Format: "<inode>/<chunk_index>" — decimal, '/' separator. Stays a single
// header so neither `chunk/` nor `vfs/` needs to include the other.
inline std::string ChunkKey(metadata::InodeID ino, metadata::ChunkIndex idx) {
  return std::to_string(ino) + "/" + std::to_string(idx);
}

}  // namespace swordfs::utils