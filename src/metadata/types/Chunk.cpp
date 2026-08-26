// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Chunk.hpp"

#include <limits>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

utils::Status SwordFsChunk::SerializeTo(std::string *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Invalid chunk record");
  }
  BufEncoder writer;
  writer.Header(RecordType::kChunk);
  writer.U32(index);
  writer.U64(start_offset);
  writer.String(key);
  writer.U64(size);
  writer.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsChunk::ParseFrom(std::string_view data, SwordFsChunk *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Chunk output is null");
  }
  BufDecoder reader(data);
  SwordFsChunk chunk;
  uint64_t size = 0;
  if (!reader.Header(RecordType::kChunk) || !reader.U32(&chunk.index) || !reader.U64(&chunk.start_offset) ||
      !reader.String(&chunk.key) || !reader.U64(&size) || size > std::numeric_limits<size_t>::max() || !reader.Done()) {
    return utils::Status::Malformed("Malformed chunk record");
  }
  chunk.size = static_cast<size_t>(size);
  *out = std::move(chunk);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
