// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// InodeHandle — per-inode handle in the VFS layer.
//
// Wraps a FileReadWriter (the pure read/write facility) and owns the
// inode's *runtime* state: the open-fd count and the orphaned flag.
// This state is per-client and must NOT live in the metadata backend
// (SwordFsInode), which is meant to be persistent.

#pragma once

#include <fcntl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs::vfs {

class FileReadWriter;

class InodeHandle {
 public:
  InodeHandle(metadata::InodeID ino, std::shared_ptr<FileReadWriter> rw);

  /// Acquire one open-fd reference and apply open-time semantics for
  /// |flags| (e.g. O_TRUNC truncates the file to zero).  Called once,
  /// right after the FileHandle is created.
  utils::Status Open(int flags);

  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  utils::Status Write(const folly::IOBuf &buf, off_t off);

  /// Always flush — used by FUSE FLUSH / FSYNC.
  utils::Status Flush();

  /// Release one open-fd reference.  Flushes and reclaims when the last
  /// reference is released.
  utils::Status Close();

  /// Mark this inode as unlinked while still open.
  void MarkOrphaned();

  /// Return true if at least one open fd references this handle.
  bool IsOpen() const;

  metadata::InodeID ino() const { return ino_; }

  /// Number of open file descriptors referencing this handle.
  uint64_t open_count() const { return open_count_.load(); }

  // Exposed for unit-test access only.
  const std::shared_ptr<FileReadWriter> &rw() const { return rw_; }

 private:
  metadata::InodeID ino_;
  std::shared_ptr<FileReadWriter> rw_;
  std::atomic<uint64_t> open_count_{0};
  bool orphaned_ = false;
};

// Opaque map type — defined in InodeHandle.cpp.
struct InodeHandleMap;

// InodeHandleManager — singleton mapping inode → InodeHandle.  FileHandle
// delegates InodeHandle creation here; the metadata layer looks up an
// InodeHandle by inode number when it needs to mark it orphaned.
class InodeHandleManager {
 public:
  static InodeHandleManager &Instance();

  /// Return the shared InodeHandle for |ino|.  Creates it (and its
  /// FileReadWriter) when |create_if_missing| is true.  Returns nullptr
  /// when |create_if_missing| is false and no handle exists.
  std::shared_ptr<InodeHandle> Get(metadata::InodeID ino, bool create_if_missing);

 private:
  InodeHandleManager();

  mutable std::shared_mutex mutex_;
  std::unique_ptr<InodeHandleMap> inode_handles_;
};

}  // namespace swordfs::vfs
