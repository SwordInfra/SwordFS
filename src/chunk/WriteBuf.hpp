// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// WriteBuf — single-chunk write buffer.  Owned by a Chunk, sealed and
// uploaded when full.

#pragma once

#include <folly/io/IOBuf.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::chunk {

/// Single-chunk write buffer backed by folly::IOBuf.
///
/// Holds at most |capacity| bytes of in-flight write data.
class WriteBuf {
 public:
  /// Allocates one contiguous IOBuf of exactly |capacity| bytes.
  explicit WriteBuf(size_t capacity);

  /// Write the contents of |data| at the given buffer-relative |offset|.
  utils::Status Write(off_t offset, const folly::IOBuf& data);

  /// Return all buffered data for upload.
  std::string_view FlushData() const;

  /// Copy up to |len| bytes starting at chunk-relative |off| into |out|.
  /// The number of bytes copied is available via out->length() increase.
  utils::Status CopyOut(off_t off, size_t len, folly::IOBuf *out) const;

  /// Number of valid bytes in the buffer.
  size_t size() const { return buf_ ? buf_->length() : 0; }

 private:
  std::unique_ptr<folly::IOBuf> buf_;
};

}  // namespace swordfs::chunk
