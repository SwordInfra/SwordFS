// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <cstring>

#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::chunk {

Chunk::Chunk(metadata::InodeID ino, metadata::ChunkIndex index)
    : ino_(ino),
      max_chunk_size_(volume::VolumeImpl::Instance().chunk_size()),
      wb_(volume::VolumeImpl::Instance().chunk_size()),
      state_(State::kWriting),
      index_(index),
      data_(volume::VolumeImpl::Instance().data_engine()) {}

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf &data) {
  auto status = wb_.Write(write_offset - StartOffset(), data);
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
  return wb_.CopyOut(off, len, out);
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

utils::Status Chunk::Flush() {
  if (IsFlushed() || wb_.size() == 0) {
    return utils::Status::OK();
  }

  // Not yet sealed → seal now (write-then-flush without GetNextFlushable).
  if (IsWriting()) Seal();

  auto data = wb_.CloneBuf();
  auto status = data_->Put(ChunkKey(), std::move(data));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_
                      << " chunk=" << index_
                      << " size=" << wb_.size()
                      << " — " << status.message();
    return status;  // WriteBuf data preserved, chunk now sealed.
  }

  state_ = State::kFlushed;
  SWORDFS_LOG_INFO << "Flush uploaded: ino=" << ino_
                   << " chunk=" << index_
                   << " size=" << wb_.size();
  return utils::Status::OK();
}

metadata::ChunkMeta Chunk::BuildMeta() const {
  metadata::ChunkMeta cm;
  cm.start_offset = StartOffset();
  cm.key = ChunkKey();
  cm.size = wb_.size();
  return cm;
}

}  // namespace swordfs::chunk
