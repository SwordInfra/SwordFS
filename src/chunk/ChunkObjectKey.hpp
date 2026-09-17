// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>

#include "metadata/types/Common.hpp"

namespace swordfs::chunk {

inline std::string FormatChunkObjectKey(metadata::InodeID ino, metadata::ChunkIndex index,
                                        metadata::ChunkRevision revision) {
  return std::to_string(ino) + "/" + std::to_string(index) + "/" + std::to_string(revision);
}

}  // namespace swordfs::chunk
