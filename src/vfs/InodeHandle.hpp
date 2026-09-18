// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// InodeHandle — per-inode handle in the VFS layer.
//
// Wraps a FileReadWriter (the pure read/write facility) and owns the
// inode's *runtime* state: the open-fd count, the orphaned flag, and the
// reclaim fence. This state is per-client and must NOT live in the
// metadata backend (SwordFsInode), which is meant to be persistent.
//
// The open-fd count, the orphaned flag and the reclaim fence live under one
// mutex, and ReclaimData() takes the defer-or-reclaim decision inside a single
// critical section. A reclaim can therefore never freeze an inode between
// another caller's metadata check and its descriptor reference: either the
// reference is visible to the reclaim (which then defers) or the fence is
// visible to the opener (which then fails with NotFound). The same critical
// section records the orphan marking, so the reclaim deferred to the last
// Close() can never be lost to a stale decision. The durable orphan candidate
// itself lives in the metadata engine (published by unlink/rename-overwrite);
// the orphaned flag here is only the per-client deferral state that routes
// that candidate to the last Close().

#pragma once

#include <fcntl.h>

#include <cstdint>
#include <memory>

#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

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

  utils::Status SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields, metadata::SwordFsInode *out);

  /// Always flush — used by FUSE FLUSH / FSYNC.
  utils::Status Flush();

  /// Release one open-fd reference.  Flushes and reclaims when the last
  /// reference is released.
  utils::Status Close();

  /// Fully reclaim this inode: claim the reclaim fence, have the metadata
  /// engine freeze the inode's object identities (its point of no return),
  /// delete those objects from the data engine, and complete the reclaim.
  /// The single entry point for cleanup once an inode is no longer reachable
  /// from any directory entry — unlink, rename-overwrite and the reclaim
  /// reconciliation all come through here.
  ///
  /// The defer-or-reclaim decision is taken atomically with the open-fd count
  /// and the reclaim fence, so it can never act on stale state:
  ///   - a descriptor still holds the inode: it is marked orphaned on this
  ///     handle and the last Close() performs the reclaim;
  ///   - a reclaim or a final Close already owns the fence: the orphan marking
  ///     is preserved and this returns OK, leaving the inode to that owner
  ///     (and, if it fails, to reconciliation);
  ///   - otherwise: the fence is claimed here and the reclaim runs now.
  ///
  /// The fence is released once the attempt finishes, so a revived inode
  /// stays openable and a failed attempt can be retried (by the periodic
  /// reconciliation or by a later call).
  utils::Status ReclaimData();

  metadata::InodeID ino() const {
    return ino_;
  }

  /// Number of open file descriptors referencing this handle.
  // Exposed for unit-test access only.
  uint64_t open_count() const;

  // Exposed for unit-test access only.
  const std::shared_ptr<FileReadWriter> &rw() const {
    return rw_;
  }

 private:
  // Result of ReleaseRef: whether this release dropped the open-fd count to
  // zero, whether the inode was orphaned (unlinked while open) at that
  // moment, and whether the release also claimed the reclaim fence.
  struct ReleaseState {
    bool is_last;
    bool orphaned;
    bool fence_claimed;
  };

  // Acquires one open-fd reference under state_mutex_, unless the inode is
  // already fenced for reclaim. The check and the increment are one critical
  // section, so a concurrent ReclaimData cannot freeze the inode in between.
  bool AcquireRefUnlessReclaiming();

  // Begin a descriptor close. When more than one descriptor reference exists,
  // this drops the caller's reference immediately and returns false. When the
  // caller owns the last reference, it deliberately leaves that reference
  // counted and returns true so the descriptor remains live while Close()
  // flushes. A concurrent Open may therefore still acquire a reference during
  // the flush, while unlink/reconciliation observes a live descriptor and
  // defers reclaim instead of freezing data mid-flush.
  bool PrepareClose();

  // Releases one descriptor reference after a last-close flush or after an
  // Open failure. When |allow_reclaim| is true and this release makes the
  // count zero for an orphaned inode, it claims the reclaim fence in the same
  // critical section so the caller can perform the cleanup exactly once. The
  // local orphaned flag is consumed when the final reference disappears;
  // durable retry state lives in metadata from that point onward.
  ReleaseState ReleaseRef(bool allow_reclaim);

  // Reclaims while this handle already holds the fence (claimed by the last
  // ReleaseRef or by ReclaimData), then releases it.
  utils::Status ReclaimWithFence();

  // Releases one descriptor reference on an open failure path, completing the
  // orphan reclaim if this was the last reference of an unlinked inode.
  void ReleaseRefAfterFailedOpen();

  // Releases the fence once the reclaim attempt finished, whether it was
  // prepared, declined (the inode was already reclaimed or a Link revived
  // it), or failed part-way. A released fence never lets an open reach a
  // deleted inode: after preparation the inode no longer exists.
  void ReleaseReclaim();

  metadata::InodeID ino_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  std::shared_ptr<FileReadWriter> rw_;
  mutable utils::FiberMutex state_mutex_;
  uint64_t open_count_{0};
  bool orphaned_ = false;
  bool reclaim_started_ = false;
};

// Opaque map type — defined in InodeHandle.cpp.
struct InodeHandleMap;

// InodeHandleManager — registry mapping inode → InodeHandle.
class InodeHandleManager {
 public:
  static InodeHandleManager &Instance();

  /// (Re)initialize the registry — clears all per-inode state. Called from
  /// the mount's FUSE init hook (fiber domain, before mount-time reclaim
  /// reconciliation walks the registry), and by unit-test SetUp to drop
  /// leaked InodeHandles from a prior test (which would otherwise leave
  /// stale open-counts and reclaim fences that make later reclaims refuse
  /// arbitrary inodes).
  void Initialize();

  /// Return the shared InodeHandle for |ino|. Creates it (and its
  /// FileReadWriter) when |create_if_missing| is true. Returns nullptr
  /// when |create_if_missing| is false and no handle exists.
  std::shared_ptr<InodeHandle> Get(metadata::InodeID ino, bool create_if_missing);

 private:
  InodeHandleManager();

  mutable utils::FiberMutex mutex_;
  std::unique_ptr<InodeHandleMap> inode_handles_;
};

}  // namespace swordfs::vfs
