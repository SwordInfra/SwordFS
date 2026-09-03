// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileHandle — per-open file handle.

#pragma once

#include <memory>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "vfs/Handle.hpp"
#include "vfs/InodeHandle.hpp"

namespace swordfs {
namespace volume {
class VolumeImpl;
}
}  // namespace swordfs

namespace swordfs::vfs {

class FileHandle : public Handle {
 public:
  FileHandle() = default;
  /// Construct a handle bound to |handle|. The fh is assigned when the
  /// handle is registered with HandleManager.
  explicit FileHandle(std::shared_ptr<InodeHandle> handle);

  /// Open a regular file and register the new handle with HandleManager.
  static utils::Status Open(metadata::InodeID ino, int flags, std::shared_ptr<FileHandle> *out);

  utils::Status Release();

  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  utils::Status Write(const folly::IOBuf &buf, off_t off);

  /// Always flush — used by FUSE FLUSH / FSYNC.
  utils::Status Flush();

  // Exposed for HandleManager and unit tests.
  const std::shared_ptr<InodeHandle> &handle() const {
    return handle_;
  }

 private:
  std::shared_ptr<InodeHandle> handle_;
};

}  // namespace swordfs::vfs
