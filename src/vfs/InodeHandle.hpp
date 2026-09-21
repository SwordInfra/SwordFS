// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// InodeHandle — per-inode handle in the VFS layer.
//
// Wraps a FileReadWriter and owns only per-mount runtime state: the open-fd
// count and a reclaim fence. Durable metadata is the sole authority for
// whether reclaim work exists; this handle merely prevents the background
// reclaimer from crossing its metadata point of no return while a local file
// descriptor is live or being opened.

#pragma once

#include <fcntl.h>

#include <cstdint>
#include <memory>

#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs::vfs {

class FileReadWriter;
class LiveAttrGuard;

class InodeHandle {
 public:
  explicit InodeHandle(metadata::InodeID ino);

  /// Open one more file descriptor on this inode.
  utils::Status Open(int flags);

  /// Acquire the descriptor reference for an inode created by the same FUSE
  /// CREATE request, without re-entering existing-inode metadata Open logic.
  utils::Status OpenCreated();

  utils::Status Read(size_t size, off_t off, folly::IOBuf *out);

  utils::Status Write(const folly::IOBuf &buf, off_t off);

  utils::Status GetAttr(metadata::SwordFsInode *out) const;

  /// Hold the inode's shared operation lock across an external metadata read
  /// and subsequent local live-attribute composition.
  LiveAttrGuard LockLiveAttr() const;

  utils::Status SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields, metadata::SwordFsInode *out);

  /// Always flush — used by FUSE FLUSH / FSYNC.
  utils::Status Flush();

  /// Release one open-fd reference. The last reference remains counted until
  /// its flush completes so background reclaim cannot freeze data mid-flush.
  utils::Status Close();

  /// Claim the local reclaim fence iff no descriptor reference is live and no
  /// other reclaim already owns it. The background Reclaimer is the only
  /// production caller. Once claimed, concurrent Open() fails until
  /// FinishReclaim() releases the fence.
  bool TryStartReclaim();

  /// Release a fence previously claimed by TryStartReclaim().
  void FinishReclaim();

  metadata::InodeID ino() const {
    return ino_;
  }

  /// Number of open file descriptors referencing this handle.
  // Exposed for unit-test access only.
  uint64_t open_count() const;

 private:
  // Acquires one open-fd reference under state_mutex_, unless the inode is
  // already fenced for reclaim. The check and the increment are one critical
  // section, so a concurrent background reclaim cannot freeze the inode in
  // between.
  bool AcquireRefUnlessReclaiming();

  // Begin a descriptor close. When more than one descriptor reference exists,
  // this drops the caller's reference immediately and returns false. When the
  // caller owns the last reference, it deliberately leaves that reference
  // counted and returns true so the descriptor remains live while Close()
  // flushes. A concurrent Open may therefore still acquire a reference during
  // the flush, while unlink/reconciliation observes a live descriptor and
  // defers reclaim instead of freezing data mid-flush.
  bool PrepareClose();

  // Release one descriptor reference after a last-close flush or failed Open.
  void ReleaseRef();

  metadata::InodeID ino_;
  std::shared_ptr<FileReadWriter> rw_;
  mutable utils::FiberMutex state_mutex_;
  uint64_t open_count_{0};
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
