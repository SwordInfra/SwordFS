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

  /// Mark this inode as orphaned (nlink==0) if at least one fd is still
  /// open. Returns true when at least one fd is still open (reclaim is
  /// then deferred to the last Close()); returns false when no fds are
  /// open, so the caller must reclaim immediately.
  bool MarkOrphanedIfOpen();

  metadata::InodeID ino() const { return ino_; }

  /// Number of open file descriptors referencing this handle.
  // Exposed for unit-test access only.
  uint64_t open_count() const;

  // Exposed for unit-test access only.
  const std::shared_ptr<FileReadWriter> &rw() const { return rw_; }

 private:
  // Result of ReleaseRef: whether this release dropped the open-fd count
  // to zero, and whether the inode was orphaned (unlinked while open) at
  // that moment. Both values are captured under state_mutex_ in a single
  // critical section.
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

// InodeHandleManager — singleton mapping inode → InodeHandle. FileHandle
// delegates InodeHandle creation here; the VFS layer (VfsImpl::Unlink,
// InodeHandle::Close) consults the manager to decide whether the inode
// has any open file descriptors before scheduling deferred reclaim.
class InodeHandleManager {
 public:
  static InodeHandleManager &Instance();

  // Return true if any open file descriptor currently references |ino|.
  bool HasOpenHandles(metadata::InodeID ino);

  // Mark the ino's InodeHandle as orphaned if any fd still references
  // it. Safe to call when no open fds exist (returns without effect).
  void MarkOrphaned(metadata::InodeID ino);

  // Fully reclaim an orphaned inode: enumerate the chunk keys the
  // metadata engine has registered for |ino|, delete the corresponding
  // chunk objects from the data engine, then ask the metadata engine
  // to drop the inode itself. This is the single entry point that the
  // VFS layer uses to clean up an unlinked-but-live inode — both
  // `VfsImpl::Unlink` (when no fd is open) and `InodeHandle::Close`
  // (when the last fd goes away on an orphaned inode) funnel through it.
  //
  // Per-chunk data-engine delete failures are logged but do not abort the
  // cleanup; the metadata engine still drops the inode so the filesystem
  // view stays consistent. A background garbage collector (not yet
  // implemented) is expected to pick up stranded objects.
  static utils::Status ReclaimData(metadata::InodeID ino);

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
