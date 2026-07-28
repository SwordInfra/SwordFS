// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/WriteBuf.hpp"

#include <algorithm>
#include <cstring>

namespace swordfs::chunk {

WriteBuf::WriteBuf(size_t capacity) {
  buf_ = folly::IOBuf::create(capacity);
}

utils::Status WriteBuf::Write(const char* data, size_t size, off_t write_offset) {
  off_t end = write_offset + static_cast<off_t>(size);
  // Bounds check — writes must stay within the pre-allocated capacity.
  if (write_offset < 0) {
    return utils::Status::InvalidArgument("WriteBuf: negative write offset");
  } else if (end > static_cast<off_t>(buf_->capacity())) {
    return utils::Status::InvalidArgument(
        "WriteBuf: write exceeds capacity");
  }

  // ── Unavoidable copy ─────────────────────────────────────────
  // The source buffer belongs to the FUSE layer.  Once
  // fuse_reply_write() returns, the kernel may reuse or free that
  // memory immediately.  We must own a private copy so the data
  // survives until the chunk is sealed and uploaded.
  uint8_t* dest = const_cast<uint8_t*>(buf_->data()) + write_offset;
  std::memcpy(dest, data, size);

  if (end > static_cast<off_t>(buf_->length())) {
    buf_->append(end - buf_->length());
  }

  return utils::Status::OK();
}

std::string_view WriteBuf::FlushData() const {
  if (!buf_ || buf_->length() == 0) return {};
  return {reinterpret_cast<const char*>(buf_->data()), buf_->length()};
}

utils::Status WriteBuf::CopyOut(off_t off, size_t len, folly::IOBuf* out) const {
  if (off < 0) {
    return utils::Status::InvalidArgument("CopyOut: negative offset");
  }
  if (static_cast<size_t>(off) >= buf_->length()) {
    return utils::Status::OK();  // EOF — nothing to copy
  }
  size_t avail = buf_->length() - static_cast<size_t>(off);
  size_t n = std::min(len, avail);
  if (n > out->tailroom()) {
    return utils::Status::InvalidArgument(
        "CopyOut: output buffer too small");
  }
  std::memcpy(out->writableTail(), buf_->data() + off, n);
  out->append(n);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk
