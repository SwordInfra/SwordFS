// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <cstring>

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
      meta_(volume::VolumeImpl::Instance().meta_engine()) {}

utils::Status Chunk::Initialize() {
  metadata::ChunkMeta cm;
  auto status = meta_->FindChunk(ino_, index_, &cm);
  if (status.ok()) {
    state_ = State::kFlushed;
    flushed_size_ = cm.size;
    return Status::OK();
  } else if (status.IsNotFound()) {
    // create a chunk but not commit to metadata so only the local mount knows it.
    state_ = State::kWriting;
    wb_ = std::make_unique<WriteBuf>(volume::VolumeImpl::Instance().chunk_size());
    return Status::OK();
  }
  return status;
}

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf &data) {
  if (!IsWriting()) {
    return utils::Status::InvalidArgument("Chunk::Write: chunk is sealed or flushed");
  }
  auto status = wb_->Write(write_offset - StartOffset(), data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Write FAILED: ino=" << ino_
                      << " index=" << index_ << " write_offset=" << write_offset
                      << " data_size=" << data.length() << " — " << status.message();
  }
  return status;
}

utils::Status Chunk::Read(off_t off, size_t len, folly::IOBuf *out) const {
  if (out->tailroom() < len) {
    return utils::Status::InvalidArgument("Chunk::Read: output buffer too small");
  }
  if (IsFlushed()) {
    return data_->Get(ChunkKey(), off, len, out);
  }
  return wb_->CopyOut(off, len, out);
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

utils::Status Chunk::Flush() {
  if (IsFlushed() || !wb_ || wb_->size() == 0) {
    return utils::Status::OK();
  }

  if (IsWriting()) {
    Seal();
  }

  flushed_size_ = wb_->size();
  auto data = wb_->CloneBuf();
  auto status = data_->Put(ChunkKey(), std::move(data));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_
                      << " chunk=" << index_
                      << " size=" << flushed_size_
                      << " — " << status.message();
    return status;
  }

  // free the dirty data buffer
  state_ = State::kFlushed;
  wb_.reset();

  // Register with the metadata engine so future reads can locate
  // this chunk via FindChunk.
  meta_->AddChunk(ino_, BuildMeta());

  SWORDFS_LOG_DEBUG << "Flush uploaded: ino=" << ino_
                    << " chunk=" << index_
                    << " size=" << flushed_size_;
  return utils::Status::OK();
}

metadata::ChunkMeta Chunk::BuildMeta() const {
  metadata::ChunkMeta cm;
  cm.index = index_;
  cm.key = ChunkKey();
  cm.size = IsFlushed() ? flushed_size_ : (wb_ ? wb_->size() : 0);
  return cm;
}

}  // namespace swordfs::chunk
