// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <vector>

#include "chunk/slice/SliceRecord.hpp"
#include "metadata/ChunkMetadata.hpp"

namespace swordfs::metadata::slice {

// Slice-private metadata: an independent ordered record stream per ChunkID.
// AppendRecord is not a file/EOF publication protocol.
class SliceChunkMetadata : public ChunkMetadata {
 public:
  ChunkType Type() const final {
    return ChunkType::kChunkSlice;
  }

  virtual utils::Status AllocateSliceID(chunk::slice::SliceID *out) = 0;
  // Persist the first record before FileMetadata attaches the ChunkID.
  // There is no empty authoritative Slice index.
  virtual utils::Status CreateIndex(ChunkID chunk_id, const chunk::slice::SliceRecord &first_record) = 0;
  virtual utils::Status ReadRecords(ChunkID chunk_id, std::vector<chunk::slice::SliceRecord> *out) = 0;
  virtual utils::Status AppendRecord(ChunkID chunk_id, const chunk::slice::SliceRecord &record) = 0;
};

}  // namespace swordfs::metadata::slice
