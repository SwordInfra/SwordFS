// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileHandleManager — singleton mapping fh → FileReadWriter.
// Thread-safe: all public methods acquire the appropriate lock.

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "vfs/FileReadWriter.hpp"

namespace swordfs {
namespace volume {
class VolumeImpl;
}
}  // namespace swordfs

namespace swordfs::vfs {

struct FileHandle {
  std::shared_ptr<FileReadWriter> file_readwriter;
};

class FileHandleManager {
 public:
  static FileHandleManager &Instance();

  /// Allocate a file handle, create a FileReadWriter from the global
  /// VolumeImpl, and return the handle via |*fh|.
  utils::Status Open(metadata::InodeID ino, uint64_t *fh);

  /// Allocate a directory handle mapped to |ino|.
  uint64_t OpenDir(metadata::InodeID ino);

  /// Release a directory handle.
  void ReleaseDir(uint64_t fh);

  /// Find the FileHandle for |fh|.  Returns std::nullopt if not found.
  /// The returned FileHandle keeps the underlying FileReadWriter alive.
  /// When a handle is returned, file_readwriter is guaranteed non-null.
  std::optional<FileHandle> Find(uint64_t fh);

  /// Flush and remove |fh|.  Called on release / close.
  void Release(uint64_t fh);

 private:
  FileHandleManager() = default;

  uint64_t AllocFh();

  mutable std::shared_mutex mutex_;
  uint64_t next_fh_{1};
  folly::F14FastMap<uint64_t, FileHandle> files_;
  folly::F14FastMap<uint64_t, metadata::InodeID> dir_handles_;
};

}  // namespace swordfs::vfs
