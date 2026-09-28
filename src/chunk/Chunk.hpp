// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs::chunk {

// One mount-local logical chunk. Mechanism-specific publication, private
// metadata and cleanup details stay behind concrete implementations.
class Chunk {
 public:
  virtual ~Chunk() = default;

  metadata::ChunkIndex Index() const {
    return index_;
  }

  virtual utils::Status Read(size_t offset, size_t len, folly::IOBuf *out) const = 0;
  virtual utils::Status Write(size_t offset, const folly::IOBuf &data) = 0;
  virtual utils::Status Flush() = 0;
  virtual void TruncateLocal(size_t size) = 0;
  virtual bool HasPendingWrites() const = 0;

 protected:
  explicit Chunk(metadata::ChunkIndex index) : index_(index) {
  }

 private:
  metadata::ChunkIndex index_;
};

}  // namespace swordfs::chunk
