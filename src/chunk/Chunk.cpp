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
  if (IsFlushed()) {
    const auto &published = PublishedChunk();
    return data_->Get(FormatChunkObjectKey(ino_, index_, published.revision), off, len, out);
  }
  return wb_->CopyOut(off, len, out);
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

std::vector<std::string> Chunk::ObjectKeysForCleanup() const {
  std::vector<std::string> keys;
  if (published_chunk_) {
    keys.push_back(FormatChunkObjectKey(ino_, index_, published_chunk_->revision));
  }
  if (pending_revision_ != metadata::kInvalidChunkRevision) {
    // Allocated revisions are globally unique within the volume, so a pending
    // revision can never name the same object as the published revision.
    keys.push_back(FormatChunkObjectKey(ino_, index_, pending_revision_));
  }
  return keys;
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

  auto cleanup_pending_object = [&] {
    auto delete_status = data_->Delete(chunk_key);
    if (!delete_status.ok()) {
      SWORDFS_LOG_ERROR << "Chunk::Flush conflict cleanup FAILED: key=" << chunk_key << " — "
                        << delete_status.message();
    }
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
        if (current.revision != chunk_meta.revision) {
          cleanup_pending_object();
        }
        return utils::Status::AlreadyExists("Chunk::Flush: published chunk changed before publication at index " +
                                            std::to_string(index_));
      }
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

  status = meta_->CommitChunk(ino_, expected, chunk_meta);
  if (!status.ok()) {
    if (status.IsAlreadyExists()) {
      cleanup_pending_object();
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
  return meta_->AllocateChunkRevision(&pending_revision_);
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
  state_ = State::kWriting;
  return utils::Status::OK();
}

void Chunk::CompletePublication(const metadata::SwordFsChunk &chunk) {
  std::optional<std::string> old_key;
  if (published_chunk_) {
    old_key = FormatChunkObjectKey(ino_, index_, published_chunk_->revision);
  }

  published_chunk_ = chunk;
  pending_revision_ = metadata::kInvalidChunkRevision;
  state_ = State::kFlushed;
  wb_.reset();

  if (old_key) {
    auto status = data_->Delete(*old_key);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Chunk::CompletePublication old-object cleanup FAILED: key=" << *old_key << " — "
                        << status.message();
    }
  }
}

}  // namespace swordfs::chunk
