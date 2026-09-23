// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::chunk {

struct WholeObjectRef {
  metadata::InodeID ino = 0;
  metadata::SwordFsChunk descriptor;
  std::string key;

  bool operator==(const WholeObjectRef &) const = default;
};

// These codecs belong to the whole-object implementation. The generic
// reclaim types persist their output as opaque bytes.
utils::Status FreezeWholeObjectDelete(metadata::InodeID ino, const metadata::SwordFsChunk &chunk, uint64_t chunk_size,
                                      metadata::PendingDelete *out);
utils::Status FreezeWholeObjectReclaim(metadata::InodeID ino, const std::vector<metadata::SwordFsChunk> &chunks,
                                       uint64_t chunk_size, metadata::ReclaimWork *out);
utils::Status DecodeWholeObjectDelete(const metadata::PendingDelete &work, uint64_t chunk_size, WholeObjectRef *out);
utils::Status DecodeWholeObjectReclaim(const metadata::ReclaimWork &work, uint64_t chunk_size,
                                       std::vector<WholeObjectRef> *out);

}  // namespace swordfs::chunk
