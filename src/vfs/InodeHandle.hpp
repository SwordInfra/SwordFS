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

#include <cstdint>
#include <memory>
#include <mutex>
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
  explicit InodeHandle(metadata::InodeID ino);

  /// Open one more file descriptor on this inode.
  utils::Status Open(int flags);

  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  utils::Status Write(const folly::IOBuf &buf, off_t off);

  /// Always flush — used by FUSE FLUSH / FSYNC.
  utils::Status Flush();

  /// Release one open-fd reference.  Flushes and reclaims when the last
  /// reference is released.
  utils::Status Close();

  /// Atomically mark this inode as orphaned (nlink==0) and report whether
  /// open fds remain.  Returns true when at least one fd is still open, in
  /// which case reclaim is deferred to the last Close(); returns false when
  /// no fds are open, so the caller must reclaim immediately.
  bool MarkOrphanedIfOpen();

  metadata::InodeID ino() const { return ino_; }

  /// Number of open file descriptors referencing this handle.
  // Exposed for unit-test access only.
  uint64_t open_count() const;

  // Exposed for unit-test access only.
  const std::shared_ptr<FileReadWriter> &rw() const { return rw_; }

 private:
  // Result of ReleaseRef: whether this release dropped the open-fd count to
  // zero, and whether the inode was orphaned (unlinked while open) at that
  // moment.  Both are captured under state_mutex_ in a single atomic step.
  struct ReleaseState {
    bool is_last;
    bool orphaned;
  };

  // Acquires one open-fd reference under state_mutex_.
  void AcquireRef();

  // Releases one open-fd reference and returns the resulting state, so
  // Close() can flush on the last reference and reclaim exactly once when
  // the last reference to an orphaned inode is released.
  ReleaseState ReleaseRef();

  metadata::InodeID ino_;
  std::shared_ptr<FileReadWriter> rw_;
  mutable std::mutex state_mutex_;
  uint64_t open_count_{0};
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
