// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS metadata — abstract interface for inode and directory
// operations. First implementation is in-memory (MemMetaImpl); a TiKV-backed
// implementation will follow.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Reclaim.hpp"
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
using InodeVisitorFn = std::function<Status(InodeID)>;
using ReclaimVisitorFn = std::function<Status(const ReclaimWork &)>;
using PendingDeleteVisitorFn = std::function<Status(const PendingDelete &)>;

/// Well-known metadata engine URLs.
constexpr std::string_view kMemoryMetaUrl = "memory://local";

/// Concurrency and execution-domain contract:
///
/// - engine construction/destruction plus Initialize/FormatVolume/LoadVolume
///   are lifecycle/control operations and execute in the POSIX-thread domain;
/// - runtime filesystem operations execute in the fiber domain;
/// - every runtime operation must be atomic with respect to concurrent
///   observers. Concurrent callers must never see an intermediate state of a
///   composite operation (e.g. a Rename whose target has been unlinked but
///   whose source has not yet been moved). For KV-backed implementations each
///   operation is expected to map onto a single transaction where required.
///
/// Implementations should validate these semantic boundaries directly rather
/// than relying on an incidental mutex acquisition to detect misuse.
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

  /// POSIX unlink(2): detach the directory entry and decrement nlink. When the
  /// last name of a file disappears, the same atomic metadata mutation
  /// publishes the durable orphan candidate consumed by the reclaim worker.
  virtual Status Unlink(InodeID parent_ino, std::string_view name) = 0;

  /// Remove an empty directory. Decrements parent nlink.
  virtual Status RmDir(InodeID parent_ino, std::string_view name) = 0;

  /// Rename (move) an entry between directories. |flags| is a bitwise OR of
  /// RenameFlag values. If an overwritten file loses its last name, the same
  /// atomic metadata mutation publishes its durable orphan candidate.
  virtual Status Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                        std::string_view new_name, RenameFlag flags) = 0;

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

  /// Prepare the reclaim of |ino| and return the frozen object identities
  /// that must be deleted from the data engine.
  ///
  /// This is the inode's reclaim point of no return. In one atomic mutation
  /// the engine must:
  ///   - recheck that |ino| is still orphaned (nlink == 0);
  ///   - freeze the authoritative chunk descriptors and their immutable
  ///     object keys into a durable pending-reclaim record;
  ///   - drop the live inode (and its orphan marker) so a concurrent Link
  ///     can no longer revive an inode whose objects are about to go away.
  ///
  /// On success, |*work| contains the frozen work when the point of no return
  /// was crossed (or was crossed by an earlier replay). An empty optional is
  /// the ordinary no-op outcome: the inode was already reclaimed, is not a
  /// reclaimable file, or a concurrent Link revived it. In that case no
  /// object may be deleted. Backend failures remain Status errors rather than
  /// being overloaded into the no-op outcome.
  ///
  /// The memory backend mirrors these semantics for the lifetime of the
  /// process; persistent backends must persist the pending record so that
  /// mount-time reconciliation can finish the job after a crash.
  virtual Status PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) = 0;

  /// Remove the durable pending-reclaim record for |ino| together with its
  /// frozen chunk metadata. Called only after every object identity returned
  /// by PrepareReclaim has been deleted. Idempotent: a missing record is not
  /// an error.
  virtual Status CompleteReclaim(InodeID ino) = 0;

  /// Visit every inode currently published as an orphan candidate — that is,
  /// every inode whose nlink dropped to zero and which has not been reclaimed
  /// or revived since. The candidate is published atomically by the mutation
  /// that reached nlink == 0 (unlink, rename-overwrite), so a crash before
  /// the caller reclaims it cannot lose the inode. Persisted by persistent
  /// backends; process-lifetime for the memory backend.
  virtual Status VisitOrphanCandidates(const InodeVisitorFn &visitor) = 0;

  /// Visit every durable pending reclaim. The visitor receives the frozen work
  /// itself so replay can continue object deletion directly without another
  /// metadata lookup or PrepareReclaim call.
  virtual Status VisitPendingReclaims(const ReclaimVisitorFn &visitor) = 0;

  /// Visit a bounded batch of immutable object identities registered as
  /// best-effort cleanup candidates after rewrite/truncate publication.
  /// A candidate is not delete authority: consumers must revalidate current
  /// authoritative metadata before physical deletion. |max_items| is a hard
  /// bound on visitor invocations. After arguments are validated, implementations
  /// reset |has_more| to false before scanning; on OK it reports whether additional
  /// work is already known to remain in the current scan cycle.
  virtual Status VisitPendingDeletesBatch(size_t max_items, const PendingDeleteVisitorFn &visitor, bool *has_more) = 0;

  /// Remove one pending-delete key after its object has been deleted.
  /// Idempotent: a missing key is not an error.
  virtual Status CompletePendingDelete(std::string_view key) = 0;

  /// Allocate a globally unique, monotonically increasing chunk revision.
  /// Revisions are volume-scoped persistent identities: a backend must never
  /// return zero or reuse an allocated revision while metadata for that volume
  /// remains authoritative. Allocated-but-unpublished revisions may be skipped
  /// after failures.
  virtual Status AllocateChunkRevision(ChunkRevision *revision) = 0;

  /// Open a directory and create its per-open iterator. Implementations may
  /// share backend directory-entry prefetch/cache state between iterators.
  virtual Status OpenDir(InodeID ino, DirIteratorPtr *iterator) = 0;

  // ────────────────────────────────────────────────────────────────
  // Chunk metadata
  // ────────────────────────────────────────────────────────────────

  /// Atomically publish |replacement| as the authoritative descriptor.
  ///
  /// When |expected| is empty, the chunk must not already exist (first
  /// publication). When |expected| is present, the current descriptor must
  /// match it (rewrite CAS). Replaying an already-published |replacement| is
  /// idempotent and must also reconcile inode side effects after an ambiguous
  /// or partially applied backend transaction. A rewrite replacement revision
  /// must be strictly greater than the expected revision. Publication must
  /// never shrink inode size. A conflicting descriptor returns AlreadyExists;
  /// a rewrite whose expected descriptor disappeared returns NotFound.
  ///
  /// Cleanup completeness is deliberately outside the publication contract.
  /// Backends should best-effort register an obsolete |expected| revision, or
  /// a definitely rejected uploaded |replacement|, for background cleanup.
  /// Failure to register cleanup may leak an object but must not invalidate an
  /// otherwise known publication result. Conversely, an ambiguous backend
  /// error does not prove publication failed; callers must retain the uploaded
  /// candidate and reconcile authoritative metadata before treating it as
  /// obsolete.
  virtual Status CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                             const SwordFsChunk &replacement) = 0;

  /// Find the chunk at |idx|.  Returns OK and fills |*chunk| if a
  /// matching chunk is registered for the given inode.
  virtual Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) = 0;

  /// Truncate |ino| to |size| bytes. Updates the inode size and drops chunk
  /// metadata beyond the new size. Backends should best-effort register the
  /// detached immutable objects for background cleanup; registration failure
  /// may leak garbage but does not invalidate a known-success truncate.
  virtual Status Truncate(InodeID ino, uint64_t size) = 0;
};

}  // namespace swordfs::metadata
