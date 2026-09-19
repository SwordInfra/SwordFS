// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <cstring>

#include "chunk/ChunkObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::chunk {

Chunk::Chunk(metadata::InodeID ino, metadata::ChunkIndex index)
    : ino_(ino),
      max_chunk_size_(volume::VolumeImpl::Instance().chunk_size()),
      index_(index),
      data_(volume::VolumeImpl::Instance().data_engine()),
      meta_(volume::VolumeImpl::Instance().meta_engine()) {
}

utils::Status Chunk::Initialize() {
  metadata::SwordFsChunk chunk;
  auto status = meta_->FindChunk(ino_, index_, &chunk);
  if (status.ok()) {
    state_ = State::kFlushed;
    published_chunk_ = chunk;
    return Status::OK();
  } else if (status.IsNotFound()) {
    // create a chunk but not commit to metadata so only the local mount knows it.
    state_ = State::kWriting;
    wb_ = std::make_unique<WriteBuf>(max_chunk_size_);
    return Status::OK();
  }
  return status;
}

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf &data) {
  if (IsFlushed()) {
    auto status = HydrateForWrite();
    if (!status.ok()) {
      return status;
    }
  }
  if (!IsWriting()) {
    return utils::Status::InvalidArgument("Chunk::Write: chunk is sealed");
  }
  auto status = wb_->Write(write_offset - StartOffset(), data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Write FAILED: ino=" << ino_ << " index=" << index_ << " write_offset=" << write_offset
                      << " data_size=" << data.length() << " — " << status.message();
  }
  return status;
}

utils::Status Chunk::Read(off_t off, size_t len, folly::IOBuf *out) const {
  if (out->tailroom() < len) {
    return utils::Status::InvalidArgument("Chunk::Read: output buffer too small");
  }
  if (len == 0) {
    return utils::Status::OK();
  }

  const size_t original_length = out->length();
  utils::Status status;
  if (IsFlushed()) {
    const auto &published = PublishedChunk();
    status = data_->Get(FormatChunkObjectKey(ino_, index_, published.revision), off, len, out);
  } else {
    status = wb_->CopyOut(off, len, out);
  }

  const size_t bytes_read = out->length() - original_length;
  if (!status.ok()) {
    out->trimEnd(bytes_read);
    return status;
  }
  if (bytes_read != len) {
    out->trimEnd(bytes_read);
    return utils::Status::IOError("Chunk::Read: backend returned a short read");
  }
  return utils::Status::OK();
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

void Chunk::Truncate(size_t size) {
  if (IsFlushed()) {
    if (published_chunk_) {
      published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
    }
    return;
  }
  if (wb_) {
    wb_->Truncate(size);
  }
  if (published_chunk_) {
    // A rewrite remembers the old descriptor as its CAS expectation. Metadata
    // truncate clamps that descriptor too, so keep the local expectation in
    // lockstep for a later flush/retry.
    published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
  }
}

utils::Status Chunk::Flush() {
  if (IsFlushed() || !wb_ || wb_->size() == 0) {
    return utils::Status::OK();
  }

  const bool retrying = !IsWriting();
  if (IsWriting()) {
    Seal();
  }

  auto status = EnsurePendingRevision();
  if (!status.ok()) {
    return status;
  }

  const auto chunk_meta = BuildMeta();
  const auto chunk_key = FormatChunkObjectKey(ino_, index_, chunk_meta.revision);
  const auto expected = published_chunk_;

  auto relinquish_rejected_revision = [&] {
    // A known publication rejection makes this uploaded immutable revision
    // terminal. Metadata may have registered it for best-effort cleanup, so a
    // later Flush must allocate a fresh revision rather than republishing a key
    // that the background Reclaimer may delete.
    pending_revision_ = metadata::kInvalidChunkRevision;
    pending_object_uploaded_ = false;
  };

  if (retrying) {
    // A failed/ambiguous publication leaves the chunk sealed. Resolve the
    // authoritative descriptor before uploading again: a committed previous
    // attempt can be completed idempotently, while a competing publication
    // must not be overwritten or have its live object deleted.
    metadata::SwordFsChunk current;
    status = meta_->FindChunk(ino_, index_, &current);
    if (status.ok()) {
      if (current == chunk_meta) {
        status = meta_->CommitChunk(ino_, expected, chunk_meta);
        if (!status.ok()) {
          return status;
        }
        CompletePublication(chunk_meta);
        return utils::Status::OK();
      }

      const bool current_matches_expected = expected.has_value() && current == *expected;
      if (!current_matches_expected) {
        if (pending_object_uploaded_) {
          // Re-enter CommitChunk so metadata can classify the already-uploaded
          // revision against the current authoritative state. If the CAS can
          // still complete, the object is known durable so publication remains
          // safe; a known rejection makes the revision terminal locally.
          status = meta_->CommitChunk(ino_, expected, chunk_meta);
          if (status.ok()) {
            CompletePublication(chunk_meta);
            return utils::Status::OK();
          }
        } else {
          status = utils::Status::AlreadyExists("Chunk::Flush: published chunk changed before publication at index " +
                                                std::to_string(index_));
        }
        if ((status.IsAlreadyExists() || status.IsNotFound()) && current.revision != chunk_meta.revision &&
            pending_object_uploaded_) {
          relinquish_rejected_revision();
        }
        return status;
      }
    } else if (status.IsNotFound() && expected.has_value() && pending_object_uploaded_) {
      // The expected descriptor disappeared after an earlier successful Put.
      // Route the definite rejection through CommitChunk so the backend can
      // best-effort register cleanup after it confirms the publication result.
      status = meta_->CommitChunk(ino_, expected, chunk_meta);
      if (status.ok()) {
        CompletePublication(chunk_meta);
        return utils::Status::OK();
      }
      if (status.IsAlreadyExists() || status.IsNotFound()) {
        relinquish_rejected_revision();
      }
      return status;
    } else if (!status.IsNotFound() || expected.has_value()) {
      return status;
    }
  }

  auto data = wb_->CloneBuf();
  status = data_->Put(chunk_key, std::move(data));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_ << " chunk=" << index_ << " size=" << chunk_meta.size
                      << " — " << status.message();
    return status;
  }
  pending_object_uploaded_ = true;

  status = meta_->CommitChunk(ino_, expected, chunk_meta);
  if (!status.ok()) {
    if (status.IsAlreadyExists() || status.IsNotFound()) {
      relinquish_rejected_revision();
    }
    SWORDFS_LOG_ERROR << "Chunk::Flush CommitChunk FAILED: ino=" << ino_ << " chunk=" << index_
                      << " size=" << chunk_meta.size << " — " << status.message();
    return status;
  }

  CompletePublication(chunk_meta);

  SWORDFS_LOG_DEBUG << "Flush uploaded: ino=" << ino_ << " chunk=" << index_ << " size=" << chunk_meta.size;
  return utils::Status::OK();
}

metadata::SwordFsChunk Chunk::BuildMeta() const {
  metadata::SwordFsChunk chunk;
  chunk.index = index_;
  chunk.start_offset = static_cast<uint64_t>(StartOffset());
  chunk.revision = pending_revision_;
  chunk.size = wb_->size();
  return chunk;
}

const metadata::SwordFsChunk &Chunk::PublishedChunk() const {
  CHECK(published_chunk_.has_value());
  return *published_chunk_;
}

utils::Status Chunk::EnsurePendingRevision() {
  if (pending_revision_ != metadata::kInvalidChunkRevision) {
    return utils::Status::OK();
  }
  auto status = meta_->AllocateChunkRevision(&pending_revision_);
  if (status.ok()) {
    pending_object_uploaded_ = false;
  }
  return status;
}

utils::Status Chunk::HydrateForWrite() {
  const auto &published = PublishedChunk();
  if (published.size > max_chunk_size_) {
    return utils::Status::Malformed("Chunk::HydrateForWrite: published chunk exceeds configured chunk size");
  }

  auto wb = std::make_unique<WriteBuf>(max_chunk_size_);
  if (published.size > 0) {
    auto persisted = folly::IOBuf::create(published.size);
    auto status =
        data_->Get(FormatChunkObjectKey(ino_, index_, published.revision), 0, published.size, persisted.get());
    if (!status.ok()) {
      return status;
    }
    if (persisted->length() != published.size) {
      return utils::Status::IOError("Chunk::HydrateForWrite: persisted object is shorter than metadata descriptor");
    }
    status = wb->Write(0, *persisted);
    if (!status.ok()) {
      return status;
    }
  }

  wb_ = std::move(wb);
  pending_revision_ = metadata::kInvalidChunkRevision;
  pending_object_uploaded_ = false;
  state_ = State::kWriting;
  return utils::Status::OK();
}

void Chunk::CompletePublication(const metadata::SwordFsChunk &chunk) {
  published_chunk_ = chunk;
  pending_revision_ = metadata::kInvalidChunkRevision;
  pending_object_uploaded_ = false;
  state_ = State::kFlushed;
  wb_.reset();
}

}  // namespace swordfs::chunk
