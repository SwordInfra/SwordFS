// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/cow/COWChunk.hpp"

#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

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

}  // namespace

COWChunk::COWChunk(metadata::InodeID ino, metadata::ChunkIndex index, size_t max_chunk_size,
                   metadata::cow::COWChunkMetadataPtr cow_metadata, metadata::IMetaEngine *meta,
                   storage::IDataEngine *data, std::optional<metadata::SwordFsChunk> published_chunk)
    : Chunk(index),
      ino_(ino),
      max_chunk_size_(max_chunk_size),
      cow_metadata_(std::move(cow_metadata)),
      meta_(meta),
      data_(data),
      published_chunk_(published_chunk) {
  CHECK(meta_ != nullptr);
  CHECK(data_ != nullptr);
  CHECK_GT(max_chunk_size_, 0);
  if (published_chunk_.has_value()) {
    CHECK_EQ(published_chunk_->index, Index());
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
    CHECK(published_chunk_.has_value());
    std::shared_ptr<WriteBuf> hydrated;
    status = HydrateForWrite(*published_chunk_, &hydrated);
    if (!status.ok()) {
      return status;
    }
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

  CHECK(published_chunk_.has_value());
  const auto published = *published_chunk_;
  const size_t available = offset < published.size ? std::min<uint64_t>(len, published.size - offset) : 0;
  const size_t original_length = out->length();
  if (available != 0) {
    // Keep the published revision pinned by the shared chunk lock until its
    // remote read completes. A rewrite cannot make this immutable object
    // reclaimable while a mount-local read is still using it.
    status =
        data_->Get(FormatCOWObjectKey(ino_, Index(), published.revision), static_cast<off_t>(offset), available, out);
    const size_t bytes_read = out->length() - original_length;
    if (!status.ok()) {
      out->trimEnd(bytes_read);
      return status;
    }
    if (bytes_read != available) {
      out->trimEnd(bytes_read);
      return utils::Status::IOError("COWChunk::Read: backend returned a short read");
    }
  }
  AppendZeros(len - available, out);
  return utils::Status::OK();
}

void COWChunk::TruncateLocal(size_t size) {
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

utils::Status COWChunk::Flush() {
  std::shared_ptr<WriteBuf> generation;
  std::optional<metadata::SwordFsChunk> expected;
  bool refresh_baseline = false;
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
    expected = published_chunk_;
    refresh_baseline = refresh_publication_baseline_;
  }

  auto fail = [&](utils::Status failure) {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    CHECK(state_ == State::kFlushing);
    CHECK(flushing_wb_ == generation);
    flushing_wb_.reset();
    state_ = State::kDirty;
    refresh_publication_baseline_ = true;
    return failure;
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
  const auto chunk_key = FormatCOWObjectKey(ino_, Index(), replacement.revision);
  auto data = generation->CloneBuf();
  status = data_->Put(chunk_key, std::move(data));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "COWChunk::Flush FAILED: ino=" << ino_ << " chunk=" << Index() << " size=" << replacement.size
                      << " — " << status.message();
    return fail(status);
  }

  status = meta_->CommitChunk(ino_, expected, replacement, metadata::ChunkPublishIntent{});
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "COWChunk::Flush CommitChunk FAILED: ino=" << ino_ << " chunk=" << Index()
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

  SWORDFS_LOG_DEBUG << "Flush uploaded: ino=" << ino_ << " chunk=" << Index() << " size=" << replacement.size;
  return utils::Status::OK();
}

bool COWChunk::HasPendingWrites() const {
  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  return state_ != State::kClean && wb_ != nullptr && wb_->size() > 0;
}

metadata::SwordFsChunk COWChunk::BuildMeta(metadata::ChunkRevision revision, size_t size) const {
  metadata::SwordFsChunk chunk;
  chunk.index = Index();
  chunk.revision = revision;
  chunk.size = size;
  return chunk;
}

utils::Status COWChunk::LoadPublicationBaseline(std::optional<metadata::SwordFsChunk> *out) const {
  metadata::SwordFsChunk current;
  auto status = meta_->FindChunk(ino_, Index(), &current);
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

utils::Status COWChunk::HydrateForWrite(const metadata::SwordFsChunk &published, std::shared_ptr<WriteBuf> *out) const {
  if (published.size > max_chunk_size_) {
    return utils::Status::Malformed("COWChunk::HydrateForWrite: published chunk exceeds configured chunk size");
  }

  auto wb = std::make_shared<WriteBuf>(max_chunk_size_);
  if (published.size > 0) {
    auto persisted = folly::IOBuf::create(published.size);
    auto status = data_->Get(FormatCOWObjectKey(ino_, Index(), published.revision), 0, published.size, persisted.get());
    if (!status.ok()) {
      return status;
    }
    if (persisted->length() != published.size) {
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
