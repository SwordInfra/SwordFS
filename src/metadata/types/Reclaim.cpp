// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Reclaim.hpp"

#include <limits>
#include <utility>

#include "chunk/ChunkObjectKey.hpp"
#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

utils::Status ReclaimWork::SerializeTo(std::string *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Invalid reclaim record");
  }
  if (ino == 0) {
    return utils::Status::InvalidArgument("Invalid reclaim record: missing inode");
  }
  if (chunks.size() > std::numeric_limits<uint32_t>::max()) {
    return utils::Status::InvalidArgument("Invalid reclaim record: too many chunks");
  }

  BufEncoder enc;
  enc.Header(RecordType::kReclaim);
  enc.U64(ino);
  enc.U32(static_cast<uint32_t>(chunks.size()));
  for (const auto &chunk : chunks) {
    if (chunk.descriptor.revision == kInvalidChunkRevision) {
      return utils::Status::InvalidArgument("Invalid reclaim record: chunk revision is invalid");
    }
    enc.U32(chunk.descriptor.index);
    enc.U64(chunk.descriptor.start_offset);
    enc.U64(chunk.descriptor.revision);
    enc.U64(chunk.descriptor.size);
    enc.String(chunk.key);
  }
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status ReclaimWork::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  dec.Header(RecordType::kReclaim);
  uint64_t ino = 0;
  uint32_t count = 0;
  dec.U64(&ino);
  dec.U32(&count);
  if (!dec || ino == 0) {
    return utils::Status::Malformed("Malformed reclaim record");
  }

  // Each entry consumes a bounded number of bytes, so a corrupt |count| cannot
  // spin here: the decoder fails as soon as the buffer runs out.
  std::vector<ReclaimChunk> chunks;
  for (uint32_t i = 0; i < count; ++i) {
    ReclaimChunk chunk;
    dec.U32(&chunk.descriptor.index);
    dec.U64(&chunk.descriptor.start_offset);
    dec.U64(&chunk.descriptor.revision);
    dec.U64(&chunk.descriptor.size);
    if (!dec.String(&chunk.key)) {
      return utils::Status::Malformed("Malformed reclaim record: truncated object identity");
    }
    // A frozen identity is only trustworthy when it is exactly the identity
    // the authoritative descriptor derives. A mismatching key (corruption,
    // tampering) could otherwise delete another inode's live object.
    if (chunk.descriptor.revision == kInvalidChunkRevision ||
        chunk.key != chunk::FormatChunkObjectKey(ino, chunk.descriptor.index, chunk.descriptor.revision)) {
      return utils::Status::Malformed("Malformed reclaim record: object identity mismatch");
    }
    chunks.push_back(std::move(chunk));
  }
  if (!dec.Done()) {
    return utils::Status::Malformed("Malformed reclaim record: trailing data");
  }

  this->ino = ino;
  this->chunks = std::move(chunks);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
