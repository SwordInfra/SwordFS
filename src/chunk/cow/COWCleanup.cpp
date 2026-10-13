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

  // Each detached identity needs two U64 fields. Reject overstated counts
  // before reserving memory from an untrusted maintenance payload.
  constexpr size_t kDetachedIdentityBytes = 2 * sizeof(uint64_t);
  if (count > work.payload.size() / kDetachedIdentityBytes) {
    return utils::Status::Malformed("typed COW reclaim count exceeds encoded payload");
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

}  // namespace

utils::Status FreezeCOWDetachedReclaim(metadata::InodeID ino, const std::vector<metadata::ChunkMapping> &mappings,
                                       metadata::ReclaimWork *out) {
  if (out == nullptr || ino == 0 || mappings.size() > std::numeric_limits<uint32_t>::max()) {
    return utils::Status::InvalidArgument("invalid typed COW reclaim");
  }
  metadata::BufEncoder encoded;
  encoded.Header(metadata::RecordType::kCowTypedCleanup);
  encoded.U32(static_cast<uint32_t>(COWCleanupKind::kDetachedReclaim));
  encoded.U64(ino);
  encoded.U32(static_cast<uint32_t>(mappings.size()));
  for (const auto &mapping : mappings) {
    if (!metadata::cow::internal::IsValidChunkID(mapping.chunk_id)) {
      return utils::Status::InvalidArgument("invalid attached ChunkID for typed reclaim");
    }
    encoded.U64(mapping.index);
    encoded.U64(mapping.chunk_id.Value());
  }
  metadata::ReclaimWork frozen;
  frozen.ino = ino;
  encoded.Finish(&frozen.payload);
  *out = std::move(frozen);
  return utils::Status::OK();
}

utils::Status FreezeCOWDetachedDelete(metadata::InodeID ino, const metadata::ChunkMapping &mapping,
                                      metadata::PendingDelete *out) {
  if (out == nullptr || ino == 0 || !metadata::cow::internal::IsValidChunkID(mapping.chunk_id)) {
    return utils::Status::InvalidArgument("invalid detached COW cleanup identity");
  }
  metadata::BufEncoder encoded;
  encoded.Header(metadata::RecordType::kCowTypedCleanup);
  encoded.U32(static_cast<uint32_t>(COWCleanupKind::kDetachedChunk));
  encoded.U64(ino);
  encoded.U64(mapping.index);
  encoded.U64(mapping.chunk_id.Value());
  metadata::PendingDelete pending;
  pending.id = "cow:chunk:" + std::to_string(mapping.chunk_id.Value());
  encoded.Finish(&pending.payload);
  *out = std::move(pending);
  return utils::Status::OK();
}

utils::Status FreezeCOWRevisionDelete(metadata::ChunkID id, metadata::cow::COWChunkRevision revision,
                                      metadata::PendingDelete *out) {
  if (out == nullptr || !ValidTypedRevision({id, revision})) {
    return utils::Status::InvalidArgument("invalid typed COW revision cleanup candidate");
  }
  metadata::BufEncoder encoded;
  encoded.Header(metadata::RecordType::kCowTypedCleanup);
  encoded.U32(static_cast<uint32_t>(COWCleanupKind::kRevision));
  encoded.U64(id.Value());
  encoded.U64(revision.Value());
  metadata::PendingDelete pending;
  pending.id = "cow:revision:" + std::to_string(id.Value()) + ":" + std::to_string(revision.Value());
  encoded.Finish(&pending.payload);
  *out = std::move(pending);
  return utils::Status::OK();
}

namespace {

class COWCleanupParticipant final : public internal::ChunkCleanupParticipant {
 public:
  COWCleanupParticipant(std::shared_ptr<metadata::cow::COWChunkMetadata> chunk_metadata, storage::IDataEngine *data,
                        internal::ChunkReachabilityProbeFn reachability_probe)
      : chunk_metadata_(std::move(chunk_metadata)), data_(data), reachability_probe_(std::move(reachability_probe)) {
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
    if (!recognized) {
      return utils::Status::Malformed("unsupported COW pending-delete payload");
    }
    if (typed.kind == COWCleanupKind::kRevision) {
      return DeleteTypedRevision(typed.revision, completed);
    }
    return DeleteDetachedChunk(typed.detached, completed);
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
    if (!recognized) {
      return utils::Status::Malformed("unsupported COW reclaim payload");
    }
    return DeleteDetachedReclaim(typed, completed);
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
  std::shared_ptr<metadata::cow::COWChunkMetadata> chunk_metadata_;
  storage::IDataEngine *data_;
  internal::ChunkReachabilityProbeFn reachability_probe_;
};

}  // namespace

utils::Status CreateCOWCleanupParticipant(uint64_t /*chunk_size*/, metadata::ChunkMetadataPtr chunk_metadata,
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
  *out = std::make_unique<COWCleanupParticipant>(std::move(cow_metadata), data, std::move(reachability_probe));
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::cow
