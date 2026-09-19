// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Chunk.hpp"

#include <limits>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

bool SwordFsChunk::IsValidForChunkSize(uint64_t chunk_size) const {
  if (chunk_size == 0 || revision == kInvalidChunkRevision || size > chunk_size) {
    return false;
  }
  if (index != 0 && chunk_size > std::numeric_limits<uint64_t>::max() / index) {
    return false;
  }
  const uint64_t expected_start = static_cast<uint64_t>(index) * chunk_size;
  return start_offset == expected_start && size <= std::numeric_limits<uint64_t>::max() - start_offset;
}

utils::Status SwordFsChunk::SerializeTo(std::string *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Invalid chunk record");
  }
  if (revision == kInvalidChunkRevision) {
    return utils::Status::InvalidArgument("Invalid chunk revision");
  }
  BufEncoder enc;
  enc.Header(RecordType::kChunk);
  enc.U32(index);
  enc.U64(start_offset);
  enc.U64(revision);
  enc.U64(size);
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsChunk::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  dec.Header(RecordType::kChunk);
  dec.U32(&index);
  dec.U64(&start_offset);
  dec.U64(&revision);
  dec.U64(&size);
  if (!dec || !dec.Done() || revision == kInvalidChunkRevision) {
    return utils::Status::Malformed("Malformed chunk record");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
