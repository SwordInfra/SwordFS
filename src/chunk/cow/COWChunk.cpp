// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/cow/COWChunk.hpp"

#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "chunk/cow/COWCleanup.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"

namespace swordfs::chunk::cow {
namespace {

utils::Status ValidateRange(size_t offset, size_t len, size_t chunk_size, std::string_view operation) {
  if (offset > chunk_size || len > chunk_size - offset) {
    return utils::Status::InvalidArgument(std::string(operation) + ": range crosses chunk boundary");
  }
  return utils::Status::OK();
}

void AppendZeros(size_t len, folly::IOBuf *out) {
  if (len == 0) {
    return;
  }
  std::memset(out->writableTail(), 0, len);
  out->append(len);
}

std::string ObjectKey(metadata::ChunkID id, metadata::cow::COWChunkRevision revision) {
  const COWObjectKey key(id, revision);
  return std::string(static_cast<std::string_view>(key));
}

metadata::FileSizePrecondition FilePrecondition(const metadata::FileChunkSnapshot &snapshot) {
  return metadata::FileSizePrecondition{.eof = snapshot.inode.attr.size, .boundary = snapshot.eof_boundary};
}

}  // namespace

COWChunk::COWChunk(metadata::InodeID ino, metadata::ChunkIndex index, size_t max_chunk_size,
                   metadata::cow::COWChunkMetadataPtr cow_metadata, metadata::IMetaEngine *meta,
                   storage::IDataEngine *data, std::optional<metadata::ChunkID> attached_id,
                   std::optional<metadata::cow::COWChunkHead> published_head, size_t visible_prefix)
    : Chunk(index),
      ino_(ino),
      max_chunk_size_(max_chunk_size),
      cow_metadata_(std::move(cow_metadata)),
      meta_(meta),
      data_(data),
      attached_id_(attached_id),
      published_head_(published_head),
      visible_prefix_(visible_prefix) {
  CHECK(meta_ != nullptr);
  CHECK(data_ != nullptr);
  CHECK(cow_metadata_ != nullptr);
  CHECK_GT(max_chunk_size_, 0);
  CHECK_EQ(attached_id_.has_value(), published_head_.has_value());
  if (attached_id_.has_value() && published_head_.has_value()) {
    // Both identities are checked together: a half-populated session is
    // rejected by the invariant above and neither optional is unchecked.
    state_ = State::kClean;
  } else {
    wb_ = std::make_shared<WriteBuf>(max_chunk_size_);
    state_ = State::kDirty;
  }
}

utils::Status COWChunk::Write(size_t offset, const folly::IOBuf &data) {
  auto status = ValidateRange(offset, data.length(), max_chunk_size_, "COWChunk::Write");
  if (!status.ok()) {
    return status;
  }

  std::unique_lock<utils::FiberRWMutex> lock(mutex_);
  if (state_ == State::kClean) {
    CHECK(attached_id_.has_value() && published_head_.has_value());
    if (published_head_->size > max_chunk_size_ || visible_prefix_ > max_chunk_size_) {
      return utils::Status::Malformed("COWChunk::HydrateForWrite: published chunk exceeds configured chunk size");
    }
    // A cached clean session must not hydrate a retired physical revision.
    metadata::cow::COWChunkHead current;
    status = cow_metadata_->GetHead(*attached_id_, &current);
    if (status.IsNotFound()) {
      return utils::Status::Malformed("COWChunk::Write: attached ChunkID has no private COW head");
    }
    if (!status.ok()) {
      return status;
    }
    std::shared_ptr<WriteBuf> hydrated;
    status = HydrateForWrite(*attached_id_, current, &hydrated);
    if (!status.ok()) {
      return status;
    }
    published_head_ = current;
    wb_ = std::move(hydrated);
    state_ = State::kDirty;
  }

  if (state_ == State::kFlushing && wb_ == flushing_wb_) {
    auto next = std::make_shared<WriteBuf>(max_chunk_size_);
    auto snapshot = wb_->CloneBuf();
    const auto copy_status = next->Write(0, *snapshot);
    CHECK(copy_status.ok()) << "same-capacity COW copy must fit the destination buffer";
    wb_ = std::move(next);
  }
  // ValidateRange() above proves this write fits the fixed-size buffer.
  return wb_->Write(static_cast<off_t>(offset), data);
}

utils::Status COWChunk::ReadLocal(const WriteBuf &buffer, size_t offset, size_t len, folly::IOBuf *out) const {
  const size_t available = offset < buffer.size() ? std::min(len, buffer.size() - offset) : 0;
  if (available != 0) {
    const size_t original_length = out->length();
    const auto status = buffer.CopyOut(static_cast<off_t>(offset), available, out);
    CHECK(status.ok()) << "validated local read must fit the destination buffer";
    CHECK_EQ(out->length() - original_length, available);
  }
  AppendZeros(len - available, out);
  return utils::Status::OK();
}

utils::Status COWChunk::Read(size_t offset, size_t len, folly::IOBuf *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("COWChunk::Read: output buffer is null");
  }
  auto status = ValidateRange(offset, len, max_chunk_size_, "COWChunk::Read");
  if (!status.ok()) {
    return status;
  }
  if (out->tailroom() < len) {
    return utils::Status::InvalidArgument("COWChunk::Read: output buffer too small");
  }
  if (len == 0) {
    return utils::Status::OK();
  }

  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  if (state_ != State::kClean) {
    CHECK(wb_ != nullptr);
    return ReadLocal(*wb_, offset, len, out);
  }

  CHECK(attached_id_.has_value() && published_head_.has_value());
  // A revision is a physical layout generation, not a read snapshot. Always
  // resolve the authoritative private head for a clean read, even when this
  // COWChunk instance has remained cached across external publications.
  for (int attempt = 0; attempt < 3; ++attempt) {
    metadata::cow::COWChunkHead head;
    status = cow_metadata_->GetHead(*attached_id_, &head);
    if (status.IsNotFound()) {
      return utils::Status::Malformed("COWChunk::Read: attached ChunkID has no private COW head");
    }
    if (!status.ok()) {
      return status;
    }
    if (!metadata::cow::internal::IsValidHead(head) || head.size > max_chunk_size_ ||
        visible_prefix_ > max_chunk_size_) {
      return utils::Status::Malformed("COWChunk::Read: invalid typed private head");
    }
    const size_t visible = VisibleBytesForHeadLocked(head);
    const size_t available = offset < visible ? std::min<size_t>(len, visible - offset) : 0;
    const size_t original_length = out->length();
    if (available != 0) {
      status = data_->Get(ObjectKey(*attached_id_, head.revision), static_cast<off_t>(offset), available, out);
      const size_t bytes_read = out->length() - original_length;
      if (!status.ok()) {
        out->trimEnd(bytes_read);
        auto read_failure = status;
        metadata::cow::COWChunkHead refreshed;
        status = cow_metadata_->GetHead(*attached_id_, &refreshed);
        if (!status.ok()) {
          return status;
        }
        if (refreshed.revision == head.revision) {
          // A backend may report a deleted object as NotFound or a generic
          // I/O failure. Preserve the original error unless an authoritative
          // head advance proves that another revision can be retried.
          return read_failure;
        }
        continue;
      }
      if (bytes_read != available) {
        out->trimEnd(bytes_read);
        return utils::Status::IOError("COWChunk::Read: backend returned a short read");
      }
    }
    AppendZeros(len - available, out);
    return utils::Status::OK();
  }
  return utils::Status::Busy("COWChunk::Read: private head changed repeatedly while reading");
}

size_t COWChunk::VisibleBytesForHeadLocked(const metadata::cow::COWChunkHead &head) const {
  CHECK(published_head_.has_value());
  // The cached prefix was derived from the cached revision. It can still
  // restrict a size-only shrink of that revision, but must not hide bytes
  // published by a newer physical revision of the same logical ChunkID.
  if (head.revision != published_head_->revision) {
    return static_cast<size_t>(head.size);
  }
  return static_cast<size_t>(std::min<uint64_t>(head.size, visible_prefix_));
}

void COWChunk::ClampPublishedPrefixLocked(size_t visible_prefix) {
  visible_prefix_ = std::min(visible_prefix_, visible_prefix);
  if (published_head_.has_value()) {
    // Boundary sanitation changes the full-head CAS baseline but retains
    // the ChunkID/revision; it must not discard independent local writes.
    published_head_->size = std::min<uint64_t>(published_head_->size, visible_prefix);
  }
}

void COWChunk::ClampPublishedPrefix(size_t visible_prefix) {
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  ClampPublishedPrefixLocked(visible_prefix);
}

void COWChunk::TruncateLocal(size_t size) {
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  ClampPublishedPrefixLocked(size);
  if (state_ == State::kClean) {
    return;
  }
  if (wb_) {
    wb_->Truncate(size);
  }
}

utils::Status COWChunk::Flush() {
  std::shared_ptr<WriteBuf> generation;
  std::optional<metadata::ChunkID> session_id;
  std::optional<metadata::cow::COWChunkHead> session_head;
  {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    if (state_ == State::kClean || (state_ == State::kDirty && wb_->size() == 0)) {
      return utils::Status::OK();
    }
    if (state_ == State::kFlushing) {
      return utils::Status::Busy("COWChunk::Flush: publication already in flight");
    }

    state_ = State::kFlushing;
    flushing_wb_ = wb_;
    generation = flushing_wb_;
    session_id = attached_id_;
    session_head = published_head_;
  }

  auto fail = [&](utils::Status failure) {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    CHECK(state_ == State::kFlushing);
    CHECK(flushing_wb_ == generation);
    flushing_wb_.reset();
    state_ = State::kDirty;
    return failure;
  };

  metadata::FileChunkSnapshot file;
  auto status = meta_->ReadFileChunkSnapshot(ino_, Index(), &file);
  if (!status.ok()) {
    return fail(status);
  }
  if (file.chunk_id != session_id) {
    return fail(utils::Status::AlreadyExists("COWChunk::Flush: attachment changed; session is stale"));
  }
  if (session_id.has_value()) {
    metadata::cow::COWChunkHead current;
    status = cow_metadata_->GetHead(*session_id, &current);
    if (status.IsNotFound()) {
      return fail(utils::Status::Malformed("COWChunk::Flush: attached ChunkID has no COW head"));
    }
    if (!status.ok()) {
      return fail(status);
    }
    if (!session_head.has_value() || current != *session_head) {
      return fail(utils::Status::AlreadyExists("COWChunk::Flush: private COW head has changed"));
    }
  }

  uint64_t start = 0;
  status = metadata::CalculateChunkStartOffset(Index(), max_chunk_size_, &start);
  if (!status.ok()) {
    return fail(status);
  }
  if (generation->size() > metadata::kMaxSupportedFileSize - start) {
    return fail(utils::Status::InvalidArgument("COWChunk::Flush: file range exceeds supported size"));
  }
  const uint64_t end = start + generation->size();

  metadata::ChunkID id;
  metadata::cow::COWChunkHead replacement;
  bool finalized = false;
  // An initial attachment can lose the EOF precondition to another chunk's
  // successful Flush. Only a *known* precondition rejection with an absent
  // mapping allows re-preparation; every subsequent attempt allocates a fresh
  // ChunkID/revision/object. An ambiguous first attachment never reuses its ID.
  for (int candidate_attempt = 0; candidate_attempt < 8 && !finalized; ++candidate_attempt) {
    if (session_id.has_value()) {
      id = *session_id;
    } else {
      status = cow_metadata_->AllocateChunkID(&id);
      if (!status.ok()) {
        return fail(status);
      }
    }
    status = metadata::ValidateFileChunkWrite(Index(), id, end, max_chunk_size_);
    if (!status.ok()) {
      return fail(status);
    }

    metadata::cow::COWChunkRevision revision;
    status = cow_metadata_->AllocateRevision(id, &revision);
    if (!status.ok()) {
      return fail(status);
    }
    replacement = metadata::cow::COWChunkHead{.revision = revision, .size = generation->size()};
    status = data_->Put(ObjectKey(id, revision), generation->CloneBuf());
    if (!status.ok()) {
      return fail(status);
    }

    status = cow_metadata_->CompareExchangeHead(id, session_head, replacement);
    if (status.IsOutcomeUnknown()) {
      metadata::cow::COWChunkHead observed;
      auto probe = cow_metadata_->GetHead(id, &observed);
      if (!probe.ok() || observed != replacement) {
        return fail(status);
      }
    } else if (!status.ok()) {
      return fail(status);
    }

    // Once a private CAS succeeds for an attached ID, it remains the
    // mechanism publication point even if the later inode side effect fails.
    const auto mark_head_published = [&] {
      if (session_id.has_value()) {
        std::lock_guard<utils::FiberRWMutex> lock(mutex_);
        published_head_ = replacement;
      }
    };

    bool retry_fresh_attachment = false;
    for (int finalize_attempt = 0; finalize_attempt < 3; ++finalize_attempt) {
      const auto expected = FilePrecondition(file);
      if (session_id.has_value()) {
        status = meta_->FinalizeAttachedWrite(ino_, Index(), id, end, expected);
      } else {
        status = meta_->AttachPrepared(ino_, Index(), id, end, expected);
      }
      if (status.ok()) {
        finalized = true;
        break;
      }
      // Only definite precondition conflicts may retry from a fresh snapshot.
      // An EXEC error is terminal; an unknown reply allows read-only checks.
      if (!status.IsOutcomeUnknown() && status.ToErrno() != EEXIST) {
        mark_head_published();
        return fail(status);
      }

      metadata::FileChunkSnapshot observed;
      auto probe = meta_->ReadFileChunkSnapshot(ino_, Index(), &observed);
      if (!probe.ok()) {
        mark_head_published();
        return fail(probe);
      }
      if (!session_id.has_value()) {
        if (status.IsOutcomeUnknown()) {
          // A lost EXEC reply can be confirmed only when the exact prepared
          // attachment AND inode EOF are already authoritative. A partial
          // state is a terminal consistency anomaly, not permission to run a
          // compensating FileMetadata write or reuse the candidate.
          if (observed.chunk_id == id && observed.inode.attr.size >= end) {
            metadata::cow::COWChunkHead current;
            const auto check = cow_metadata_->GetHead(id, &current);
            if (!check.ok()) {
              return fail(check);
            }
            if (current == replacement) {
              finalized = true;
              break;
            }
          }
          return fail(status);
        }
        // Only a definite precondition conflict with a still-absent mapping
        // can restart with a fresh identity after refreshing the snapshot.
        const bool boundary_changed_at_same_eof = observed.inode.attr.size == file.inode.attr.size &&
                                                  observed.eof_boundary.has_value() && file.eof_boundary.has_value() &&
                                                  observed.eof_boundary->chunk_id != file.eof_boundary->chunk_id;
        if (status.ToErrno() == EEXIST && !observed.chunk_id.has_value() &&
            (observed.inode.attr.size > file.inode.attr.size || boundary_changed_at_same_eof)) {
          file = std::move(observed);
          retry_fresh_attachment = true;
          break;
        }
        return fail(status);
      }
      if (status.IsOutcomeUnknown()) {
        // The private CAS may be published, but the inode side effects are
        // not proven. Never run another EXEC for the uncertain request.
        mark_head_published();
        return fail(status);
      }
      if (observed.chunk_id != id) {
        return fail(utils::Status::AlreadyExists("COWChunk::Flush: attached ChunkID was replaced"));
      }

      metadata::cow::COWChunkHead current;
      probe = cow_metadata_->GetHead(id, &current);
      if (!probe.ok()) {
        return fail(probe);
      }
      if (current != replacement) {
        return fail(utils::Status::AlreadyExists("COWChunk::Flush: candidate COW head was superseded"));
      }
      if (observed.inode.attr.size < file.inode.attr.size) {
        mark_head_published();
        return fail(utils::Status::AlreadyExists("COWChunk::Flush: EOF shrank during finalization"));
      }
      file = std::move(observed);
    }
    if (!finalized && !retry_fresh_attachment) {
      mark_head_published();
      return fail(status);
    }
  }
  if (!finalized) {
    return fail(utils::Status::AlreadyExists("COWChunk::Flush: too many concurrent attachment changes"));
  }

  {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    CHECK(state_ == State::kFlushing);
    CHECK(flushing_wb_ == generation);
    attached_id_ = id;
    published_head_ = replacement;
    visible_prefix_ = generation->size();
    flushing_wb_.reset();
    if (wb_ == generation) {
      wb_.reset();
      state_ = State::kClean;
    } else {
      state_ = State::kDirty;
    }
  }

  if (session_id.has_value() && session_head.has_value() && session_head->revision != replacement.revision) {
    // Only a known-successful publication can retire its predecessor.
    // Registration is optional maintenance, never a reason to roll back a
    // private CAS that already made the new revision authoritative.
    metadata::PendingDelete retired;
    const auto frozen = FreezeCOWRevisionDelete(id, session_head->revision, &retired);
    if (frozen.ok()) {
      const auto queued = meta_->RegisterPendingDelete(retired);
      if (!queued.ok()) {
        SWORDFS_LOG_WARN << "COW rewrite cleanup registration failed: ino=" << ino_ << " chunk=" << Index()
                         << " ChunkID=" << id.Value() << " — " << queued.message();
      }
    } else {
      SWORDFS_LOG_WARN << "COW rewrite cleanup encoding failed: " << frozen.message();
    }
  }

  SWORDFS_LOG_DEBUG << "Typed COW Flush uploaded: ino=" << ino_ << " chunk=" << Index() << " ChunkID=" << id.Value()
                    << " size=" << replacement.size;
  return utils::Status::OK();
}

bool COWChunk::HasPendingWrites() const {
  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  return state_ != State::kClean && wb_ != nullptr && wb_->size() > 0;
}

utils::Status COWChunk::HydrateForWrite(metadata::ChunkID id, const metadata::cow::COWChunkHead &head,
                                        std::shared_ptr<WriteBuf> *out) const {
  if (head.size > max_chunk_size_ || visible_prefix_ > max_chunk_size_) {
    return utils::Status::Malformed("COWChunk::HydrateForWrite: published chunk exceeds configured chunk size");
  }

  auto wb = std::make_shared<WriteBuf>(max_chunk_size_);
  const size_t visible = VisibleBytesForHeadLocked(head);
  if (visible > 0) {
    auto persisted = folly::IOBuf::create(visible);
    auto status = data_->Get(ObjectKey(id, head.revision), 0, visible, persisted.get());
    if (!status.ok()) {
      return status;
    }
    if (persisted->length() != visible) {
      return utils::Status::IOError("COWChunk::HydrateForWrite: persisted object is shorter than metadata descriptor");
    }
    status = wb->Write(0, *persisted);
    if (!status.ok()) {
      return status;
    }
  }

  *out = std::move(wb);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::cow
