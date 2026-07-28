// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/WriteBuf.hpp"

#include <algorithm>
#include <string_view>

namespace swordfs::chunk {

// ────────────────────────────────────────────────────────────────
// Initialisation
// ────────────────────────────────────────────────────────────────

void WriteBuf::Init(metadata::InodeID ino, size_t max_chunk_size) {
  ino_ = ino;
  max_chunk_size_ = max_chunk_size;
  data_.clear();
  next_chunk_ = 0;
  max_write_end_ = 0;
}

// ────────────────────────────────────────────────────────────────
// Write path
// ────────────────────────────────────────────────────────────────

void WriteBuf::Append(const char* data, size_t size, off_t write_offset) {
  data_.insert(data_.end(), data, data + size);
  off_t end = write_offset + static_cast<off_t>(size);
  if (end > max_write_end_) max_write_end_ = end;
}

std::string_view WriteBuf::FlushData(bool force) const {
  if (data_.empty()) return {};

  size_t n;
  if (force) {
    n = data_.size();
  } else {
    n = std::min(data_.size(), max_chunk_size_);
  }
  return std::string_view(data_.data(), n);
}

void WriteBuf::CommitFlush(size_t flushed_size) {
  data_.erase(data_.begin(), data_.begin() + flushed_size);
  next_chunk_++;
}

// ────────────────────────────────────────────────────────────────
// Read-through support
// ────────────────────────────────────────────────────────────────

off_t WriteBuf::BufStart() const {
  return static_cast<off_t>(next_chunk_) *
         static_cast<off_t>(max_chunk_size_);
}

off_t WriteBuf::BufEnd() const {
  return BufStart() + static_cast<off_t>(data_.size());
}

size_t WriteBuf::CopyOut(off_t off, size_t size, std::string* out) const {
  size_t buf_off = static_cast<size_t>(off - BufStart());
  size_t avail = data_.size() - buf_off;
  size_t n = std::min(size, avail);
  out->append(data_.data() + buf_off, n);
  return n;
}

// ────────────────────────────────────────────────────────────────
// Misc
// ────────────────────────────────────────────────────────────────

void WriteBuf::Reset() {
  data_.clear();
  next_chunk_ = 0;
  max_write_end_ = 0;
}

}  // namespace swordfs::chunk
