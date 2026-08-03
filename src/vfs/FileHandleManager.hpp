// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileHandleManager — singleton mapping fh → FileReadWriter.
// Thread-safe: all public methods acquire the appropriate lock.

#pragma once

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

// Opaque map types — defined in FileHandleManager.cpp to avoid pulling
// Folly's F14FastMap into every translation unit that includes this header.
struct FileMap;
struct DirMap;

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
  FileHandleManager();

  uint64_t AllocFh();

  mutable std::shared_mutex mutex_;
  uint64_t next_fh_{1};
  std::unique_ptr<FileMap> files_;
  std::unique_ptr<DirMap> dir_handles_;
};

}  // namespace swordfs::vfs
