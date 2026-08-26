// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Chunk.hpp"

#include <limits>

#include "metadata/types/Serialization.hpp"

namespace swordfs::metadata {

utils::Status ChunkMeta::SerializeTo(std::string *out) const {
  using namespace types::serialization;
  if (out == nullptr || key.size() > kMaxStringLength) {
    return utils::Status::InvalidArgument("Invalid chunk record");
  }
  Writer writer;
  writer.Header(RecordType::kChunk);
  writer.U32(index);
  writer.U64(start_offset);
  writer.String(key);
  writer.U64(size);
  writer.Finish(out);
  return utils::Status::OK();
}

utils::Status ChunkMeta::ParseFrom(std::string_view data, ChunkMeta *out) {
  using namespace types::serialization;
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Chunk output is null");
  }
  Reader reader(data);
  ChunkMeta chunk;
  uint64_t size = 0;
  if (!reader.Header(RecordType::kChunk) || !reader.U32(&chunk.index) || !reader.U64(&chunk.start_offset) ||
      !reader.String(&chunk.key) || !reader.U64(&size) || size > std::numeric_limits<size_t>::max() || !reader.Done()) {
    return Malformed("chunk");
  }
  chunk.size = static_cast<size_t>(size);
  *out = std::move(chunk);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
