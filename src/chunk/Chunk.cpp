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
    wb_ = std::make_shared<WriteBuf>(max_chunk_size_);
    return Status::OK();
  }
  return status;
}

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf &data) {
  std::unique_lock<utils::FiberRWMutex> lock(mutex_);
  if (state_ == State::kClean) {
    CHECK(published_chunk_.has_value());
    std::shared_ptr<WriteBuf> hydrated;
    auto status = HydrateForWrite(*published_chunk_, &hydrated);
    if (!status.ok()) {
      return status;
    }
    wb_ = std::move(hydrated);
    state_ = State::kDirty;
  }

  if (state_ == State::kFlushing && wb_ == flushing_wb_) {
    auto next = std::make_shared<WriteBuf>(max_chunk_size_);
    auto snapshot = wb_->CloneBuf();
    auto status = next->Write(0, *snapshot);
    if (!status.ok()) {
      return status;
    }
    wb_ = std::move(next);
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

  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  if (state_ != State::kClean) {
    const size_t original_length = out->length();
    auto status = wb_->CopyOut(off, len, out);
    const size_t bytes_read = out->length() - original_length;
    if (!status.ok()) {
      out->trimEnd(bytes_read);
      return status;
    }
    if (bytes_read != len) {
      out->trimEnd(bytes_read);
      return utils::Status::IOError("Chunk::Read: local buffer is shorter than requested range");
    }
    return utils::Status::OK();
  }

  CHECK(published_chunk_.has_value());
  const auto published = *published_chunk_;
  const size_t original_length = out->length();
  // Keep the published revision pinned by the shared chunk lock until its
  // remote read completes. A rewrite cannot transition this chunk away from
  // kClean (and eventually make this immutable object reclaimable) while an
  // older local read is still using it. Other reads retain shared concurrency.
  auto status = data_->Get(FormatChunkObjectKey(ino_, index_, published.revision), off, len, out);
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
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  if (state_ == State::kClean) {
    if (published_chunk_) {
      published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
    }
    return;
  }
  if (wb_) {
    wb_->Truncate(size);
  }
  if (published_chunk_) {
    // The cached published descriptor is the first-attempt CAS baseline.
    // Metadata truncate clamps that descriptor too, so keep the local baseline
    // in lockstep until a failed Flush requires an authoritative refresh.
    published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
  }
}

utils::Status Chunk::Flush() {
  std::shared_ptr<WriteBuf> generation;
  std::optional<metadata::SwordFsChunk> expected;
  bool refresh_baseline = false;
  {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    if (state_ == State::kClean || (state_ == State::kDirty && wb_->size() == 0)) {
      return utils::Status::OK();
    }
    if (state_ == State::kFlushing) {
      return utils::Status::Busy("Chunk::Flush: publication already in flight");
    }

    state_ = State::kFlushing;
    flushing_wb_ = wb_;
    generation = flushing_wb_;
    expected = published_chunk_;
    refresh_baseline = refresh_publication_baseline_;
  }

  auto fail = [&](utils::Status status) {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    CHECK(state_ == State::kFlushing);
    CHECK(flushing_wb_ == generation);
    flushing_wb_.reset();
    state_ = State::kDirty;
    refresh_publication_baseline_ = true;
    return status;
  };

  if (refresh_baseline) {
    auto status = LoadPublicationBaseline(&expected);
    if (!status.ok()) {
      return fail(status);
    }
  }

  metadata::ChunkRevision revision = metadata::kInvalidChunkRevision;
  auto status = meta_->AllocateChunkRevision(&revision);
  if (!status.ok()) {
    return fail(status);
  }

  const auto replacement = BuildMeta(revision, generation->size());
  const auto chunk_key = FormatChunkObjectKey(ino_, index_, replacement.revision);
  auto data = generation->CloneBuf();
  status = data_->Put(chunk_key, std::move(data));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_ << " chunk=" << index_ << " size=" << replacement.size
                      << " — " << status.message();
    return fail(status);
  }

  status = meta_->CommitChunk(ino_, expected, replacement);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush CommitChunk FAILED: ino=" << ino_ << " chunk=" << index_
                      << " size=" << replacement.size << " — " << status.message();
    return fail(status);
  }

  {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    CHECK(state_ == State::kFlushing);
    CHECK(flushing_wb_ == generation);
    published_chunk_ = replacement;
    refresh_publication_baseline_ = false;
    flushing_wb_.reset();
    if (wb_ == generation) {
      wb_.reset();
      state_ = State::kClean;
    } else {
      state_ = State::kDirty;
    }
  }

  SWORDFS_LOG_DEBUG << "Flush uploaded: ino=" << ino_ << " chunk=" << index_ << " size=" << replacement.size;
  return utils::Status::OK();
}

bool Chunk::IsClean() const {
  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  return state_ == State::kClean;
}

bool Chunk::Flushable() const {
  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  return state_ == State::kDirty && wb_ != nullptr && wb_->size() > 0;
}

off_t Chunk::DataEnd() const {
  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  if (state_ == State::kClean) {
    CHECK(published_chunk_.has_value());
    return StartOffset() + static_cast<off_t>(published_chunk_->size);
  }
  CHECK(wb_ != nullptr);
  return StartOffset() + static_cast<off_t>(wb_->size());
}

metadata::SwordFsChunk Chunk::BuildMeta(metadata::ChunkRevision revision, size_t size) const {
  metadata::SwordFsChunk chunk;
  chunk.index = index_;
  chunk.start_offset = static_cast<uint64_t>(StartOffset());
  chunk.revision = revision;
  chunk.size = size;
  return chunk;
}

utils::Status Chunk::LoadPublicationBaseline(std::optional<metadata::SwordFsChunk> *out) const {
  metadata::SwordFsChunk current;
  auto status = meta_->FindChunk(ino_, index_, &current);
  if (status.ok()) {
    *out = current;
    return utils::Status::OK();
  }
  if (status.IsNotFound()) {
    out->reset();
    return utils::Status::OK();
  }
  return status;
}

utils::Status Chunk::HydrateForWrite(const metadata::SwordFsChunk &published, std::shared_ptr<WriteBuf> *out) const {
  if (published.size > max_chunk_size_) {
    return utils::Status::Malformed("Chunk::HydrateForWrite: published chunk exceeds configured chunk size");
  }

  auto wb = std::make_shared<WriteBuf>(max_chunk_size_);
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

  *out = std::move(wb);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
