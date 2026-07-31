// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileReadWriter — per-open-file read/write over a chain of chunks.
// Delegates chunk management to ChunkManager; this class only handles
// file-offset → chunk-boundary splitting.

#pragma once

#include <folly/io/IOBuf.h>

#include <cstdint>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs {
namespace vfs {

class FileReadWriter {
 public:
  using Status = utils::Status;
  using InodeID = metadata::InodeID;

  FileReadWriter(uint64_t fh, InodeID ino, size_t chunk_size);

  /// Write the contents of |buf| at |off|, splitting across chunk boundaries.
  Status Write(const folly::IOBuf &buf, off_t off);

  /// Read up to |size| bytes at |off| into |out|.
  /// |out| must be an empty IOBuf with capacity >= |size|.
  /// On success, out->length() reflects bytes actually read.
  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  /// Seal and upload all chunks for this file handle.
  Status Flush();

 private:
  uint64_t fh_;
  InodeID ino_;
  size_t chunk_size_;
};

}  // namespace vfs
}  // namespace swordfs
