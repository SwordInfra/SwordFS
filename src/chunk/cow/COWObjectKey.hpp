// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>

#include "metadata/cow/COWChunkMetadata.hpp"
#include "metadata/types/Common.hpp"

namespace swordfs::chunk::cow {

inline std::string FormatCOWObjectKey(metadata::InodeID ino, metadata::ChunkIndex index,
                                      metadata::ChunkRevision revision) {
  return std::to_string(ino) + "/" + std::to_string(index) + "/" + std::to_string(revision);
}

// Strong target object key for the #317 data-path cutover. FileMetadata
// coordinates deliberately do not participate in this COW-private identity.
class COWObjectKey {
 public:
  COWObjectKey(metadata::ChunkID chunk_id, metadata::cow::COWChunkRevision revision)
      : value_(std::to_string(chunk_id.Value()) + "/" + std::to_string(revision.Value())) {
  }

  explicit operator std::string_view() const noexcept {
    return value_;
  }

 private:
  std::string value_;
};

}  // namespace swordfs::chunk::cow
