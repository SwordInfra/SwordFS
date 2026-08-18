// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileHandle — per-open-fd handle, plus FileHandleManager, the singleton
// mapping fh → FileHandle.  Mirrors InodeHandle/InodeHandleManager.
// Thread-safe: all public manager methods acquire the appropriate lock.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "vfs/InodeHandle.hpp"

namespace swordfs {
namespace volume {
class VolumeImpl;
}
}  // namespace swordfs

namespace swordfs::vfs {

// FileHandle — per-open-fd handle.  Owns the file-handle id and a
// reference to the shared per-inode InodeHandle.  File semantics are
// delegated to the InodeHandle.
class FileHandle {
 public:
  FileHandle() = default;
  /// Construct a handle bound to |handle|, allocating a fresh fh from
  /// FileHandleManager.
  explicit FileHandle(std::shared_ptr<InodeHandle> handle);

  /// Open a regular file.  Delegates the per-inode work (metadata fetch,
  /// permission check) to the shared InodeHandle, allocates an fh via
  /// FileHandleManager, and stores the new FileHandle in |*out|.
  static utils::Status Open(metadata::InodeID ino, int flags, FileHandle *out);

  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  utils::Status Write(const folly::IOBuf &buf, off_t off);

  /// Always flush — used by FUSE FLUSH / FSYNC.
  utils::Status Flush();

  /// The file-handle id assigned by FileHandleManager.
  uint64_t fh() const { return fh_; }

  // Exposed for FileHandleManager::Release and unit tests.
  const std::shared_ptr<InodeHandle> &handle() const { return handle_; }

 private:
  uint64_t fh_ = 0;
  std::shared_ptr<InodeHandle> handle_;
};

// Opaque map types — defined in FileHandle.cpp to avoid pulling
// Folly's F14FastMap into every translation unit that includes this header.
struct FileMap;
struct DirMap;

class FileHandleManager {
 public:
  static FileHandleManager &Instance();

  /// Allocate a fresh file-handle id.  Purely an id allocator — registration
  /// is done separately via Register().
  uint64_t AllocateFh();

  /// Register |file_handle| under its fh.  Returns false if that fh is
  /// already in use.
  bool Register(FileHandle file_handle);

  /// Find the FileHandle for |fh|.  Returns std::nullopt if not found.
  std::optional<FileHandle> Find(uint64_t fh);

  /// Flush and remove |fh|.  Called on release / close.
  utils::Status Release(uint64_t fh);

  /// Allocate a directory handle mapped to |ino|.
  uint64_t OpenDir(metadata::InodeID ino);

  /// Release a directory handle.
  void ReleaseDir(uint64_t fh);

 private:
  FileHandleManager();

  mutable std::shared_mutex mutex_;
  uint64_t next_fh_{1};
  std::unique_ptr<FileMap> files_;
  std::unique_ptr<DirMap> dir_handles_;
};

}  // namespace swordfs::vfs
