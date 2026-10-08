// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/cow/COWCleanup.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/cow/COWChunkMetadata.hpp"
#include "metadata/types/BufCodec.hpp"
#include "storage/IDataEngine.hpp"

namespace swordfs::chunk::cow {
namespace {

bool ValidHead(const metadata::SwordFsChunk &head, uint64_t chunk_size) {
  return chunk_size == 0 ? head.revision != metadata::kInvalidChunkRevision : head.IsValidForChunkSize(chunk_size);
}

struct COWRevisionCleanup {
  metadata::ChunkID chunk_id;
  metadata::cow::COWChunkRevision revision;
};

struct COWDetachedChunk {
  metadata::InodeID ino = 0;
  metadata::ChunkIndex index = 0;
  metadata::ChunkID chunk_id;
};

struct TypedPendingCleanup {
  COWCleanupKind kind = COWCleanupKind::kRevision;
  COWRevisionCleanup revision;
  COWDetachedChunk detached;
};

bool ValidTypedRevision(const COWRevisionCleanup &cleanup) {
  return metadata::cow::internal::IsValidChunkID(cleanup.chunk_id) &&
         cleanup.revision != metadata::cow::kInvalidCOWChunkRevision &&
         cleanup.revision.Value() <= metadata::cow::kMaxCOWChunkRevisionValue;
}

utils::Status DecodeTypedPending(const metadata::PendingDelete &work, TypedPendingCleanup *out, bool *recognized) {
  *recognized = false;
  metadata::BufDecoder dec(work.payload);
  if (!dec.Header(metadata::RecordType::kCowTypedCleanup)) {
    return utils::Status::OK();
  }
  *recognized = true;

  uint32_t raw_kind = 0;
  dec.U32(&raw_kind);
  if (!dec) {
    return utils::Status::Malformed("typed COW cleanup kind is missing");
  }

  TypedPendingCleanup decoded;
  decoded.kind = static_cast<COWCleanupKind>(raw_kind);
  if (decoded.kind == COWCleanupKind::kRevision) {
    uint64_t chunk_id = 0;
    uint64_t revision = 0;
    dec.U64(&chunk_id);
    dec.U64(&revision);
    decoded.revision = {metadata::ChunkID(chunk_id), metadata::cow::COWChunkRevision(revision)};
    if (!dec.Done() || !ValidTypedRevision(decoded.revision)) {
      return utils::Status::Malformed("invalid typed COW revision cleanup");
    }
    const auto expected_id =
        "cow:revision:" + std::to_string(chunk_id) + ":" + std::to_string(decoded.revision.revision.Value());
    if (work.id != expected_id) {
      return utils::Status::Malformed("typed COW revision cleanup id mismatch");
    }
  } else if (decoded.kind == COWCleanupKind::kDetachedChunk) {
    uint64_t chunk_id = 0;
    dec.U64(&decoded.detached.ino);
    dec.U64(&decoded.detached.index);
    dec.U64(&chunk_id);
    decoded.detached.chunk_id = metadata::ChunkID(chunk_id);
    if (!dec.Done() || decoded.detached.ino == 0 ||
        !metadata::cow::internal::IsValidChunkID(decoded.detached.chunk_id)) {
      return utils::Status::Malformed("invalid typed COW detached cleanup");
    }
    if (work.id != "cow:chunk:" + std::to_string(chunk_id)) {
      return utils::Status::Malformed("typed COW detached cleanup id mismatch");
    }
  } else {
    return utils::Status::Malformed("typed COW pending-delete kind is invalid");
  }

  *out = decoded;
  return utils::Status::OK();
}

utils::Status DecodeTypedReclaim(const metadata::ReclaimWork &work, std::vector<COWDetachedChunk> *out,
                                 bool *recognized) {
  *recognized = false;
  metadata::BufDecoder dec(work.payload);
  if (!dec.Header(metadata::RecordType::kCowTypedCleanup)) {
    return utils::Status::OK();
  }
  *recognized = true;

  uint32_t raw_kind = 0;
  metadata::InodeID ino = 0;
  uint32_t count = 0;
  dec.U32(&raw_kind);
  dec.U64(&ino);
  dec.U32(&count);
  if (!dec || static_cast<COWCleanupKind>(raw_kind) != COWCleanupKind::kDetachedReclaim || ino == 0 ||
      ino != work.ino) {
    return utils::Status::Malformed("invalid typed COW reclaim header");
  }

  std::vector<COWDetachedChunk> chunks;
  chunks.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    COWDetachedChunk chunk;
    uint64_t chunk_id = 0;
    chunk.ino = ino;
    dec.U64(&chunk.index);
    dec.U64(&chunk_id);
    chunk.chunk_id = metadata::ChunkID(chunk_id);
    if (!dec || !metadata::cow::internal::IsValidChunkID(chunk.chunk_id)) {
      return utils::Status::Malformed("invalid typed COW reclaim identity");
    }
    chunks.push_back(chunk);
  }
  if (!dec.Done()) {
    return utils::Status::Malformed("typed COW reclaim has trailing data");
  }
  *out = std::move(chunks);
  return utils::Status::OK();
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

namespace {

class COWCleanupParticipant final : public internal::ChunkCleanupParticipant {
 public:
  COWCleanupParticipant(uint64_t chunk_size, std::shared_ptr<metadata::cow::COWChunkMetadata> chunk_metadata,
                        metadata::IMetaEngine *meta, storage::IDataEngine *data,
                        internal::ChunkReachabilityProbeFn reachability_probe)
      : chunk_size_(chunk_size),
        chunk_metadata_(std::move(chunk_metadata)),
        meta_(meta),
        data_(data),
        reachability_probe_(std::move(reachability_probe)) {
  }

  utils::Status DeletePending(const metadata::PendingDelete &work, bool *completed) override {
    if (completed == nullptr) {
      return utils::Status::InvalidArgument("COW cleanup completion output is null");
    }
    *completed = false;

    TypedPendingCleanup typed;
    bool recognized = false;
    auto status = DecodeTypedPending(work, &typed, &recognized);
    if (!status.ok()) {
      return status;
    }
    if (recognized) {
      if (typed.kind == COWCleanupKind::kRevision) {
        return DeleteTypedRevision(typed.revision, completed);
      }
      return DeleteDetachedChunk(typed.detached, completed);
    }

    COWRef ref;
    status = DecodeCOWDelete(work, chunk_size_, &ref);
    if (!status.ok()) {
      return status;
    }
    metadata::SwordFsChunk current;
    status = meta_->FindChunk(ref.ino, ref.descriptor.index, &current);
    if (status.ok()) {
      if (current.revision == ref.descriptor.revision) {
        return utils::Status::OK();
      }
    } else if (!status.IsNotFound()) {
      return status;
    }
    status = data_->Delete(ref.key);
    if (!status.ok()) {
      return status;
    }
    *completed = true;
    return utils::Status::OK();
  }

  utils::Status DeleteReclaim(const metadata::ReclaimWork &work, bool *completed) override {
    if (completed == nullptr) {
      return utils::Status::InvalidArgument("COW reclaim completion output is null");
    }
    *completed = false;

    std::vector<COWDetachedChunk> typed;
    bool recognized = false;
    auto status = DecodeTypedReclaim(work, &typed, &recognized);
    if (!status.ok()) {
      return status;
    }
    if (recognized) {
      return DeleteDetachedReclaim(typed, completed);
    }

    std::vector<COWRef> refs;
    status = DecodeCOWReclaim(work, chunk_size_, &refs);
    if (!status.ok()) {
      return status;
    }

    metadata::SwordFsInode inode;
    status = meta_->GetInode(work.ino, &inode);
    if (status.ok()) {
      // A live inode proves that this maintenance record does not represent a
      // completed logical reclaim. Drop it without touching physical data.
      *completed = true;
      return utils::Status::OK();
    }
    if (!status.IsNotFound()) {
      return status;
    }

    size_t failed = 0;
    for (const auto &ref : refs) {
      status = data_->Delete(ref.key);
      if (!status.ok()) {
        ++failed;
      }
    }
    if (failed != 0) {
      return utils::Status::IOError("COW reclaim left " + std::to_string(failed) + " object(s) undeleted");
    }
    *completed = true;
    return utils::Status::OK();
  }

 private:
  utils::Status DeleteTypedRevision(const COWRevisionCleanup &cleanup, bool *completed) {
    if (chunk_metadata_ == nullptr) {
      return utils::Status::NotSupported("typed COW cleanup metadata is not configured");
    }
    metadata::cow::COWChunkHead head;
    auto status = chunk_metadata_->GetHead(cleanup.chunk_id, &head);
    if (status.ok()) {
      // Size is deliberately ignored: immutable identity is ChunkID + revision.
      if (head.revision == cleanup.revision) {
        return utils::Status::OK();
      }
    } else if (!status.IsNotFound()) {
      return status;
    }

    const COWObjectKey key(cleanup.chunk_id, cleanup.revision);
    status = data_->Delete(static_cast<std::string_view>(key));
    if (!status.ok()) {
      return status;
    }
    *completed = true;
    return utils::Status::OK();
  }

  utils::Status RevalidateDetached(const COWDetachedChunk &cleanup, bool *detached) {
    if (!reachability_probe_) {
      return utils::Status::NotSupported("typed COW detach reachability is not configured");
    }
    std::optional<metadata::ChunkID> attached;
    auto status = reachability_probe_(cleanup.ino, cleanup.index, &attached);
    if (!status.ok()) {
      return status;
    }
    *detached = !attached.has_value() || *attached != cleanup.chunk_id;
    return utils::Status::OK();
  }

  utils::Status DeleteDetachedChunk(const COWDetachedChunk &cleanup, bool *completed) {
    bool detached = false;
    auto status = RevalidateDetached(cleanup, &detached);
    if (!status.ok()) {
      return status;
    }
    if (!detached) {
      // The FileMetadata transition did not commit. This optional handoff is
      // stale; acknowledge it without allowing it to become future delete
      // authority after an unrelated detach.
      *completed = true;
      return utils::Status::OK();
    }
    if (chunk_metadata_ == nullptr) {
      return utils::Status::NotSupported("typed COW cleanup metadata is not configured");
    }

    metadata::cow::COWChunkHead head;
    status = chunk_metadata_->GetHead(cleanup.chunk_id, &head);
    if (status.IsNotFound()) {
      *completed = true;
      return utils::Status::OK();
    }
    if (!status.ok()) {
      return status;
    }

    const COWObjectKey key(cleanup.chunk_id, head.revision);
    status = data_->Delete(static_cast<std::string_view>(key));
    if (!status.ok()) {
      return status;
    }
    status = chunk_metadata_->EraseHead(cleanup.chunk_id, head);
    if (!status.ok()) {
      return status;
    }
    *completed = true;
    return utils::Status::OK();
  }

  utils::Status DeleteDetachedReclaim(const std::vector<COWDetachedChunk> &chunks, bool *completed) {
    // Revalidate every FileMetadata mapping before any destructive operation.
    // A still-attached identity means the optional handoff was stale.
    for (const auto &chunk : chunks) {
      bool detached = false;
      auto status = RevalidateDetached(chunk, &detached);
      if (!status.ok()) {
        return status;
      }
      if (!detached) {
        *completed = true;
        return utils::Status::OK();
      }
    }
    for (const auto &chunk : chunks) {
      bool chunk_completed = false;
      auto status = DeleteDetachedChunk(chunk, &chunk_completed);
      if (!status.ok()) {
        return status;
      }
      if (!chunk_completed) {
        return utils::Status::Internal("detached COW cleanup did not complete after revalidation");
      }
    }
    *completed = true;
    return utils::Status::OK();
  }

 private:
  uint64_t chunk_size_;
  std::shared_ptr<metadata::cow::COWChunkMetadata> chunk_metadata_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  internal::ChunkReachabilityProbeFn reachability_probe_;
};

}  // namespace

utils::Status CreateCOWCleanupParticipant(uint64_t chunk_size, metadata::ChunkMetadataPtr chunk_metadata,
                                          metadata::IMetaEngine *meta, storage::IDataEngine *data,
                                          internal::ChunkReachabilityProbeFn reachability_probe,
                                          std::unique_ptr<internal::ChunkCleanupParticipant> *out) {
  if (out == nullptr || meta == nullptr || data == nullptr) {
    return utils::Status::InvalidArgument("invalid COW cleanup participant construction");
  }
  std::shared_ptr<metadata::cow::COWChunkMetadata> cow_metadata;
  if (chunk_metadata != nullptr) {
    cow_metadata = std::dynamic_pointer_cast<metadata::cow::COWChunkMetadata>(std::move(chunk_metadata));
    if (cow_metadata == nullptr) {
      return utils::Status::InvalidArgument("COW cleanup metadata type mismatch");
    }
  }
  *out = std::make_unique<COWCleanupParticipant>(chunk_size, std::move(cow_metadata), meta, data,
                                                 std::move(reachability_probe));
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::cow
