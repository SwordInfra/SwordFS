// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS metadata — abstract interface for inode and directory
// operations. First implementation is in-memory (MemMetaImpl); a TiKV-backed
// implementation will follow.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using Status = swordfs::utils::Status;
using SwordFsContext = swordfs::utils::SwordFsContext;

namespace swordfs::metadata {

// Backend-neutral directory iteration state. The iterator is an independent
// view of a directory enumeration; backend implementations may share the
// underlying prefetched directory entries between iterators. |offset| is an
// opaque FUSE directory cookie, interpreted by the iterator implementation.
class DirIterator {
 public:
  virtual ~DirIterator() = default;

  /// Reposition the iterator to |cookie|.
  virtual utils::Status Seek(uint64_t cookie) = 0;
  /// Inspect the entry at the current iterator position without advancing it.
  virtual utils::Status Peek(SwordFsEntry *entry, uint64_t *next_cookie) = 0;
  /// Advance to the position returned by the most recent successful Peek().
  /// Calling Advance() without a pending entry violates the iterator contract.
  virtual void Advance() = 0;
};

using DirIteratorPtr = std::shared_ptr<DirIterator>;
using ChunkVisitorFn = std::function<Status(const SwordFsChunk &)>;
using InodeVisitorFn = std::function<Status(InodeID)>;

/// Well-known metadata engine URLs.
constexpr std::string_view kMemoryMetaUrl = "memory://local";

/// Concurrency contract: every method on this interface must be atomic
/// and thread-safe.  Concurrent observers must never see an intermediate
/// state of a composite operation (e.g. a Rename whose target has been
/// unlinked but whose source has not yet been moved).  For KV-backed
/// implementations each method is expected to map onto a single
/// transaction.
class IMetaEngine {
 public:
  virtual ~IMetaEngine() = default;

  /// Initialize a metadata backend connection and validate backend-specific
  /// runtime prerequisites. Persistent backends should not create a volume here.
  virtual Status Initialize() = 0;

  /// Create a new metadata volume.
  virtual Status FormatVolume(const SwordFsVolume &config) = 0;

  /// Load an existing metadata volume and return its persistent volume
  /// configuration in |config|.
  virtual Status LoadVolume(SwordFsVolume *config) = 0;

  /// Return filesystem limits provided by this metadata engine.
  virtual Limits GetLimits() const = 0;

  /// Look up a child entry by name.
  virtual Status Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) = 0;

  /// Get an inode metadata snapshot.
  virtual Status GetInode(InodeID ino, SwordFsInode *out) = 0;

  /// Create a regular file.
  virtual Status Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) = 0;

  /// Create a directory. Increments parent nlink to account for "..".
  virtual Status MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) = 0;

  /// POSIX unlink(2): detach the directory entry and decrement nlink.
  /// When |result| is non-null, the implementation returns the inode that was
  /// actually detached plus its authoritative post-decrement nlink from the
  /// same atomic mutation. This avoids a Lookup-before-Unlink TOCTOU race in
  /// the VFS reclaim path.
  virtual Status Unlink(InodeID parent_ino, std::string_view name, UnlinkResult *result = nullptr) = 0;

  /// Remove an empty directory. Decrements parent nlink.
  virtual Status RmDir(InodeID parent_ino, std::string_view name) = 0;

  /// Rename (move) an entry between directories.  |flags| is a bitwise
  /// OR of RenameFlag values.  When |result| is non-null and the rename
  /// replaced an existing non-directory entry, the implementation fills
  /// |*result| as part of the same atomic mutation so the caller can
  /// reclaim the overwritten inode's data without a second lookup.
  /// Engines that cannot report the overwritten inode may leave
  /// |*result| untouched.
  virtual Status Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                        std::string_view new_name, RenameFlag flags, RenameResult *result = nullptr) = 0;

  /// Set attributes for an inode.  |fields| is a bitwise OR of
  /// SetAttrField values; only the bits set in |fields| are read from
  /// |attr| and applied to the inode.
  virtual Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) = 0;

  /// Get file system statistics.
  virtual Status StatFs(SwordFsStatFs *stbuf) = 0;

  /// Check access permissions.
  virtual Status Access(InodeID ino, uint32_t mask) = 0;

  /// Create a symbolic link.
  virtual Status Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) = 0;

  /// Create a hard link to an existing inode.
  virtual Status Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) = 0;

  /// Read the target of a symbolic link.
  virtual Status Readlink(InodeID ino, std::string *target) = 0;

  /// Open a regular file.  Performs the permission check (regular-file
  /// validation + read permission) and updates atime.
  virtual Status Open(InodeID ino) = 0;

  /// Start, refresh, and stop this mount's persistent metadata session.
  /// Persistent backends use the session to own open-file references so a
  /// crashed mount can be fenced and its references can be reconciled later.
  virtual Status StartSession() = 0;
  virtual Status RefreshSession() = 0;
  virtual Status StopSession() = 0;

  /// Persist one open-file reference owned by the current mount session.
  /// AcquireOpen must fail if the inode disappeared before the reference could
  /// be published. ReleaseOpen is idempotent and reports whether this release
  /// dropped the filesystem-wide open count to zero for an unlinked inode.
  virtual Status AcquireOpen(InodeID ino) = 0;
  virtual Status ReleaseOpen(InodeID ino, bool *reclaimable) = 0;

  /// Fence expired sessions and release all open references they still own.
  /// Implementations must make partial cleanup restart-safe and idempotent.
  virtual Status ReapStaleSessions() = 0;

  /// Prepare an unlinked inode for data reclamation. Once |ino| has nlink==0,
  /// atomically remove it from the live inode table, preserve its chunk list,
  /// and publish a durable pending-reclaim record. A linked inode is a no-op.
  /// Repeated calls are idempotent.
  virtual Status ReclaimInode(InodeID ino) = 0;

  /// Visit inodes whose last namespace link was removed and whose persistent
  /// filesystem-wide open-reference count is zero. Stale session references
  /// must be reconciled before an orphan becomes visible here.
  virtual Status VisitOrphanedInodes(const InodeVisitorFn &visitor) = 0;

  /// Visit durable, prepared reclaim jobs. The records must remain visible
  /// until CompleteReclaim succeeds.
  virtual Status VisitPendingReclaims(const InodeVisitorFn &visitor) = 0;

  /// Visit the frozen chunk list owned by a prepared reclaim job.
  virtual Status VisitReclaimChunks(InodeID ino, const ChunkVisitorFn &visitor) = 0;

  /// Remove a prepared reclaim job and its frozen chunk metadata after all
  /// data-object deletes have succeeded. Repeated calls are idempotent.
  virtual Status CompleteReclaim(InodeID ino) = 0;

  /// Visit the chunk metadata currently registered for a live |ino| without
  /// materializing the complete chunk map in the caller. Backends should
  /// stream or batch the enumeration when their storage API supports it.
  /// Returns immediately if |visitor| returns an error.
  virtual Status VisitChunks(InodeID ino, const ChunkVisitorFn &visitor) = 0;

  /// Open a directory and create its per-open iterator. Implementations may
  /// share backend directory-entry prefetch/cache state between iterators.
  virtual Status OpenDir(InodeID ino, DirIteratorPtr *iterator) = 0;

  // ────────────────────────────────────────────────────────────────
  // Chunk metadata
  // ────────────────────────────────────────────────────────────────

  /// Register a flushed chunk.  The metadata engine stores the chunk
  /// key, size, and start offset so that subsequent reads can locate
  /// the data via the data engine.
  virtual Status AddChunk(InodeID ino, const SwordFsChunk &chunk) = 0;

  /// Find the chunk at |idx|.  Returns OK and fills |*chunk| if a
  /// matching chunk is registered for the given inode.
  virtual Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) = 0;

  /// Truncate |ino| to |size| bytes.  Updates the inode size and drops
  /// chunk metadata for data beyond the new size.
  virtual Status Truncate(InodeID ino, uint64_t size) = 0;
};

}  // namespace swordfs::metadata
