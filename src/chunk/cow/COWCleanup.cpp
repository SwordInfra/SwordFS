// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/cow/COWCleanup.hpp"

#include <limits>
#include <utility>

#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/types/BufCodec.hpp"

namespace swordfs::chunk::cow {
namespace {

bool ValidHead(const metadata::SwordFsChunk &head, uint64_t chunk_size) {
  return chunk_size == 0 ? head.revision != metadata::kInvalidChunkRevision : head.IsValidForChunkSize(chunk_size);
}

utils::Status EncodeRefs(metadata::InodeID ino, const std::vector<COWRef> &refs, std::string *out) {
  if (out == nullptr || ino == 0 || refs.size() > std::numeric_limits<uint32_t>::max()) {
    return utils::Status::InvalidArgument("invalid COW cleanup references");
  }
  metadata::BufEncoder enc;
  enc.Header(metadata::RecordType::kCowCleanup);
  enc.U64(ino);
  enc.U32(static_cast<uint32_t>(refs.size()));
  for (const auto &ref : refs) {
    if (ref.ino != ino || ref.key != FormatCOWObjectKey(ino, ref.descriptor.index, ref.descriptor.revision)) {
      return utils::Status::InvalidArgument("COW cleanup reference identity mismatch");
    }
    enc.U64(ref.descriptor.index);
    enc.U64(ref.descriptor.revision);
    enc.U64(ref.descriptor.size);
    enc.String(ref.key);
  }
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status DecodeRefs(std::string_view payload, metadata::InodeID expected_ino, uint64_t chunk_size,
                         std::vector<COWRef> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("COW cleanup output is null");
  }
  metadata::BufDecoder dec(payload);
  metadata::InodeID ino = 0;
  uint32_t count = 0;
  dec.Header(metadata::RecordType::kCowCleanup);
  dec.U64(&ino);
  dec.U32(&count);
  if (!dec || ino == 0 || ino != expected_ino) {
    return utils::Status::Malformed("COW cleanup inode mismatch");
  }
  std::vector<COWRef> refs;
  for (uint32_t i = 0; i < count; ++i) {
    COWRef ref;
    ref.ino = ino;
    dec.U64(&ref.descriptor.index);
    dec.U64(&ref.descriptor.revision);
    dec.U64(&ref.descriptor.size);
    dec.String(&ref.key);
    if (!dec || !ValidHead(ref.descriptor, chunk_size) ||
        ref.key != FormatCOWObjectKey(ino, ref.descriptor.index, ref.descriptor.revision)) {
      return utils::Status::Malformed("COW cleanup identity mismatch");
    }
    refs.push_back(std::move(ref));
  }
  if (!dec.Done()) {
    return utils::Status::Malformed("COW cleanup has trailing data");
  }
  *out = std::move(refs);
  return utils::Status::OK();
}

}  // namespace

utils::Status FreezeCOWDelete(metadata::InodeID ino, const metadata::SwordFsChunk &chunk, uint64_t chunk_size,
                              metadata::PendingDelete *out) {
  if (out == nullptr || ino == 0 || !ValidHead(chunk, chunk_size)) {
    return utils::Status::InvalidArgument("invalid COW pending delete");
  }
  const auto key = FormatCOWObjectKey(ino, chunk.index, chunk.revision);
  metadata::PendingDelete work;
  work.id = "cow:" + key;
  auto status = EncodeRefs(ino, std::vector<COWRef>{{ino, chunk, key}}, &work.payload);
  if (!status.ok()) {
    return status;
  }
  *out = std::move(work);
  return utils::Status::OK();
}

utils::Status FreezeCOWReclaim(metadata::InodeID ino, const std::vector<metadata::SwordFsChunk> &chunks,
                               uint64_t chunk_size, metadata::ReclaimWork *out) {
  if (out == nullptr || ino == 0) {
    return utils::Status::InvalidArgument("invalid COW reclaim");
  }
  std::vector<COWRef> refs;
  refs.reserve(chunks.size());
  for (const auto &chunk : chunks) {
    if (!ValidHead(chunk, chunk_size)) {
      return utils::Status::InvalidArgument("invalid COW reclaim descriptor");
    }
    refs.push_back({ino, chunk, FormatCOWObjectKey(ino, chunk.index, chunk.revision)});
  }
  metadata::ReclaimWork work;
  work.ino = ino;
  auto status = EncodeRefs(ino, refs, &work.payload);
  if (!status.ok()) {
    return status;
  }
  *out = std::move(work);
  return utils::Status::OK();
}

utils::Status DecodeCOWDelete(const metadata::PendingDelete &work, uint64_t chunk_size, COWRef *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("COW delete output is null");
  }
  std::vector<COWRef> refs;
  // The inode is carried inside the private payload, not by the generic queue.
  metadata::BufDecoder dec(work.payload);
  metadata::InodeID ino = 0;
  dec.Header(metadata::RecordType::kCowCleanup);
  dec.U64(&ino);
  if (!dec || ino == 0) {
    return utils::Status::Malformed("invalid COW cleanup inode");
  }
  auto status = DecodeRefs(work.payload, ino, chunk_size, &refs);
  if (!status.ok()) {
    return status;
  }
  if (refs.size() != 1 || work.id != "cow:" + refs[0].key) {
    return utils::Status::Malformed("COW pending delete id mismatch");
  }
  *out = std::move(refs[0]);
  return utils::Status::OK();
}

utils::Status DecodeCOWReclaim(const metadata::ReclaimWork &work, uint64_t chunk_size, std::vector<COWRef> *out) {
  return DecodeRefs(work.payload, work.ino, chunk_size, out);
}

}  // namespace swordfs::chunk::cow
