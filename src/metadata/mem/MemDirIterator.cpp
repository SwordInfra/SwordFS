// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemDirIterator.hpp"

#include <folly/logging/xlog.h>

#include <utility>

namespace swordfs::metadata {

MemDirIterator::MemDirIterator(std::vector<SwordFsEntry> entries) : entries_(std::move(entries)) {
}

Status MemDirIterator::Seek(uint64_t cookie) {
  position_ = cookie;
  pending_next_.reset();
  return Status::OK();
}

Status MemDirIterator::Peek(SwordFsEntry *entry, uint64_t *next_cookie) {
  if (entry == nullptr || next_cookie == nullptr) {
    return Status::InvalidArgument("directory iterator output is null");
  }
  if (pending_next_) {
    return Status::InvalidArgument("directory iterator has pending entry");
  }
  if (position_ >= entries_.size()) {
    return Status::EndOfDirectory("directory end");
  }

  *entry = entries_[static_cast<size_t>(position_)];
  *next_cookie = position_ + 1;
  pending_next_ = *next_cookie;
  return Status::OK();
}

void MemDirIterator::Advance() {
  CHECK(pending_next_.has_value());
  position_ = *pending_next_;
  pending_next_.reset();
}

}  // namespace swordfs::metadata
