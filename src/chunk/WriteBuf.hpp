// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// WriteBuf — single-chunk write buffer.  Owned by a Chunk, sealed and
// uploaded when full.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs::chunk {

/// Single-chunk write buffer backed by folly::IOBuf.
///
/// Holds at most |capacity| bytes of in-flight write data.
class WriteBuf {
 public:
  explicit WriteBuf(size_t capacity);
  ~WriteBuf();

  // Movable (required by std::deque<Chunk>::erase)
  WriteBuf(WriteBuf&&) = default;
  WriteBuf& operator=(WriteBuf&&) = default;

  // Non-copyable (owns unique_ptr)
  WriteBuf(const WriteBuf&) = delete;
  WriteBuf& operator=(const WriteBuf&) = delete;

  /// Write the contents of |data| at the given buffer-relative |offset|.
  utils::Status Write(off_t offset, const folly::IOBuf& data);

  /// Clone the underlying IOBuf.  Shares the buffer (refcount bump),
  /// does NOT copy data.  The original buffer remains valid in the
  /// WriteBuf so a failed upload can be retried.
  std::unique_ptr<folly::IOBuf> CloneBuf() const;

  /// Copy up to |len| bytes starting at chunk-relative |off| into |out|.
  /// The number of bytes copied is available via out->length() increase.
  utils::Status CopyOut(off_t off, size_t len, folly::IOBuf *out) const;

  /// Number of valid bytes in the buffer.
  size_t size() const;

 private:
  std::unique_ptr<folly::IOBuf> buf_;
};

}  // namespace swordfs::chunk
