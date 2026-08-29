// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "metadata/types/Entry.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Backend-neutral directory iteration state. The iterator's continuation
// state is deliberately opaque to VFS; Redis can keep an HSCAN cursor while
// Memory can keep a stable snapshot. |offset| is a logical FUSE directory
// position, not a backend cursor.
class IDirIterator {
 public:
  virtual ~IDirIterator() = default;

  // Return the next entry at |offset| without consuming it. This lets VFS
  // determine whether the entry fits in the caller's byte buffer before
  // advancing the iterator.
  virtual utils::Status Peek(uint64_t offset, SwordFsEntry *entry, uint64_t *next_offset, bool *end) = 0;

  // Consume entries starting at the requested logical offset. The returned
  // entries are assigned consecutive logical positions beginning at offset;
  // |next_offset| is the position to pass to the next call. When the end is
  // reached, |next_offset| is unchanged and |end| is true.
  virtual utils::Status Read(uint64_t offset, size_t max_entries, std::vector<SwordFsEntry> *entries,
                             uint64_t *next_offset, bool *end) = 0;
};

}  // namespace swordfs::metadata
