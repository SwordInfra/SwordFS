// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

#include <cstring>

#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"

namespace swordfs::chunk {

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf& data) {
  return wb_.Write(write_offset - StartOffset(), data);
}

std::string_view Chunk::FlushData() const { return wb_.FlushData(); }

utils::Status Chunk::Read(off_t off, size_t len, folly::IOBuf *out) const {
  return wb_.CopyOut(off, len, out);
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

utils::Status Chunk::Flush() {
  if (IsWriting() && !empty()) Seal();
  if (!IsSealed()) return utils::Status::OK();

  std::string_view data = wb_.FlushData();
  if (data.empty()) return utils::Status::OK();

  std::string payload(data);
  auto status = data_->Put(ChunkKey(), payload);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_
                      << " chunk=" << index_
                      << " size=" << data.size()
                      << " — " << status.message();
    return status;
  }
  SWORDFS_LOG_INFO << "Flush uploaded: ino=" << ino_
                   << " chunk=" << index_
                   << " size=" << data.size();
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
