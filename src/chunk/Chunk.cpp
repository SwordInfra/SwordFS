// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

#include <cstring>

#include <folly/io/IOBuf.h>

#include "storage/IDataEngine.hpp"
#include <folly/logging/xlog.h>
#include "utils/Logging.hpp"

namespace swordfs::chunk {

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf& data) {
  auto status = wb_.Write(write_offset - StartOffset(), data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Write FAILED: ino=" << ino_
                      << " index=" << index_ << " write_offset=" << write_offset
                      << " data_size=" << data.length() << " — " << status.message();
  }
  return status;
}

std::string_view Chunk::FlushData() const { return wb_.FlushData(); }

utils::Status Chunk::Read(off_t off, size_t len, folly::IOBuf *out) const {
  if (IsSealed()) {
    std::string data;
    auto status = data_->Get(ChunkKey(), &data, off, len);
    if (!status.ok()) return status;
    std::memcpy(out->writableData(), data.data(), data.size());
    out->append(data.size());
    return utils::Status::OK();
  }
  return wb_.CopyOut(off, len, out);
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

utils::Status Chunk::Flush() {
  if (IsSealed() || empty()) {
    return utils::Status::OK();
  }

  std::string_view data = wb_.FlushData();
  std::string payload(data);
  auto status = data_->Put(ChunkKey(), payload);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_
                      << " chunk=" << index_
                      << " size=" << data.size()
                      << " — " << status.message();
    return status;  // Not sealed — WriteBuf data preserved, chunk still kWriting.
  }

  Seal();
  SWORDFS_LOG_INFO << "Flush uploaded: ino=" << ino_
                   << " chunk=" << index_
                   << " size=" << data.size();
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
