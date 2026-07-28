// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

namespace swordfs::chunk {

utils::Status Chunk::Write(const char* data, size_t size, off_t write_offset) {
  // Convert file-absolute offset to chunk-relative.
  return wb_.Write(data, size, write_offset - StartOffset());
}

std::string_view Chunk::FlushData() const { return wb_.FlushData(); }

utils::Status Chunk::Read(off_t off, size_t len,
                          folly::IOBuf* out) const {
  return wb_.CopyOut(off, len, out);
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

}  // namespace swordfs::chunk
