// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Chunk.hpp"

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

utils::Status SwordFsChunk::SerializeTo(std::string *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Invalid chunk record");
  }
  BufEncoder enc;
  enc.Header(RecordType::kChunk);
  enc.U32(index);
  enc.U64(start_offset);
  enc.String(key);
  enc.U64(size);
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsChunk::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  dec.Header(RecordType::kChunk);
  dec.U32(&index);
  dec.U64(&start_offset);
  dec.String(&key);
  dec.U64(&size);
  if (!dec || !dec.Done()) {
    return utils::Status::Malformed("Malformed chunk record");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
