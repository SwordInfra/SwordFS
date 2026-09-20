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
    state_ = State::kClean;
    published_chunk_ = chunk;
    return Status::OK();
  } else if (status.IsNotFound()) {
    // create a chunk but not commit to metadata so only the local mount knows it.
    state_ = State::kDirty;
    wb_ = std::make_unique<WriteBuf>(max_chunk_size_);
    return Status::OK();
  }
  return status;
}

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf &data) {
  if (IsClean()) {
    auto status = HydrateForWrite();
    if (!status.ok()) {
      return status;
    }
  }
  auto status = wb_->Write(write_offset - StartOffset(), data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Write FAILED: ino=" << ino_ << " index=" << index_ << " write_offset=" << write_offset
                      << " data_size=" << data.length() << " — " << status.message();
  } else {
    MarkLocalDataChanged();
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
  if (IsClean()) {
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

void Chunk::Truncate(size_t size) {
  if (IsClean()) {
    if (published_chunk_) {
      published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
    }
    return;
  }
  if (wb_) {
    const auto old_size = wb_->size();
    wb_->Truncate(size);
    if (wb_->size() != old_size) {
      MarkLocalDataChanged();
    }
  }
  if (published_chunk_) {
    // A rewrite remembers the old descriptor as its CAS expectation. Metadata
    // truncate clamps that descriptor too, so keep the local expectation in
    // lockstep for a later flush/retry.
    published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
  }
}

utils::Status Chunk::Flush() {
  if (IsClean()) {
    return utils::Status::OK();
  }
  if (wb_->size() == 0) {
    return utils::Status::OK();
  }

  state_ = State::kFlushing;

  auto fail = [&](utils::Status status) {
    state_ = State::kDirty;
    return status;
  };

  bool publication_complete = false;
  auto status = ReconcilePublicationAttempt(publication_complete);
  if (!status.ok()) {
    return fail(status);
  }
  if (publication_complete) {
    return utils::Status::OK();
  }

  if (!publication_attempt_) {
    status = StartPublicationAttempt();
    if (!status.ok()) {
      return fail(status);
    }
  }
  if (!publication_attempt_) {
    return fail(utils::Status::Internal("Chunk::Flush: publication attempt was not initialized"));
  }

  auto &attempt = *publication_attempt_;
  const auto chunk_key = FormatChunkObjectKey(ino_, index_, attempt.replacement.revision);
  if (!attempt.object_uploaded) {
    auto data = wb_->CloneBuf();
    status = data_->Put(chunk_key, std::move(data));
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_ << " chunk=" << index_
                        << " size=" << attempt.replacement.size << " — " << status.message();
      return fail(status);
    }
    attempt.object_uploaded = true;
  }

  status = meta_->CommitChunk(ino_, attempt.expected, attempt.replacement);
  if (!status.ok()) {
    if (status.IsAlreadyExists() || status.IsNotFound()) {
      // Metadata may already have queued this immutable key for cleanup. Never
      // reuse the same revision after a known rejection.
      attempt.revision_reusable = false;
    }
    SWORDFS_LOG_ERROR << "Chunk::Flush CommitChunk FAILED: ino=" << ino_ << " chunk=" << index_
                      << " size=" << attempt.replacement.size << " — " << status.message();
    return fail(status);
  }

  const auto published = attempt.replacement;
  CompletePublication(published);

  SWORDFS_LOG_DEBUG << "Flush uploaded: ino=" << ino_ << " chunk=" << index_ << " size=" << published.size;
  return utils::Status::OK();
}

metadata::SwordFsChunk Chunk::BuildMeta(metadata::ChunkRevision revision) const {
  metadata::SwordFsChunk chunk;
  chunk.index = index_;
  chunk.start_offset = static_cast<uint64_t>(StartOffset());
  chunk.revision = revision;
  chunk.size = wb_->size();
  return chunk;
}

const metadata::SwordFsChunk &Chunk::PublishedChunk() const {
  CHECK(published_chunk_.has_value());
  return *published_chunk_;
}

void Chunk::MarkLocalDataChanged() {
  if (!publication_attempt_) {
    return;
  }
  publication_attempt_->payload_current = false;
  publication_attempt_->revision_reusable = false;
}

utils::Status Chunk::ReconcilePublicationAttempt(bool &publication_complete) {
  publication_complete = false;
  if (!publication_attempt_) {
    return utils::Status::OK();
  }

  const auto attempt = *publication_attempt_;
  metadata::SwordFsChunk current;
  auto status = meta_->FindChunk(ino_, index_, &current);
  if (status.ok()) {
    if (current == attempt.replacement) {
      // A matching descriptor proves reader visibility, but an earlier
      // transaction may have failed after updating it and before finishing
      // inode side effects. Replay the commit before confirming publication.
      status = meta_->CommitChunk(ino_, attempt.expected, attempt.replacement);
      if (!status.ok()) {
        return status;
      }
      published_chunk_ = current;
      if (attempt.payload_current) {
        CompletePublication(current);
        publication_complete = true;
        return utils::Status::OK();
      }
      publication_attempt_.reset();
      return utils::Status::OK();
    }

    const bool current_matches_expected = attempt.expected.has_value() && current == *attempt.expected;
    if (current_matches_expected && attempt.payload_current && attempt.revision_reusable) {
      return utils::Status::OK();
    }

    // The old CAS expectation describes only the failed attempt. Under the
    // supported single-active-mount contract, the latest complete local buffer
    // remains valid and rebases on the current authoritative descriptor.
    published_chunk_ = current;
    publication_attempt_.reset();
    return utils::Status::OK();
  }

  if (status.IsNotFound()) {
    const bool expected_absent = !attempt.expected.has_value();
    if (expected_absent && attempt.payload_current && attempt.revision_reusable) {
      return utils::Status::OK();
    }

    // The previous expected descriptor is no longer authoritative. A later
    // attempt publishes the latest complete local buffer as an initial chunk.
    published_chunk_.reset();
    publication_attempt_.reset();
    return utils::Status::OK();
  }

  return status;
}

utils::Status Chunk::StartPublicationAttempt() {
  metadata::ChunkRevision revision = metadata::kInvalidChunkRevision;
  auto status = meta_->AllocateChunkRevision(&revision);
  if (!status.ok()) {
    return status;
  }

  PublicationAttempt attempt;
  attempt.expected = published_chunk_;
  attempt.replacement = BuildMeta(revision);
  publication_attempt_ = attempt;
  return utils::Status::OK();
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
  publication_attempt_.reset();
  state_ = State::kDirty;
  return utils::Status::OK();
}

void Chunk::CompletePublication(const metadata::SwordFsChunk &chunk) {
  published_chunk_ = chunk;
  publication_attempt_.reset();
  state_ = State::kClean;
  wb_.reset();
}

}  // namespace swordfs::chunk
