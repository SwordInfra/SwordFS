# Chunk publication and cleanup — authoritative typed contract

This document describes the SwordFS beta COW data path after the #317 authority
cutover and the #320 common descriptor retirement. It specifies the currently
implemented behavior; it is **not** a migration or dual-format compatibility
contract. `cow` is the only selectable chunk mechanism.

## Authority and identity

FileMetadata and ChunkMetadata are distinct correctness domains:

| Domain | Authoritative record | Owner |
| --- | --- | --- |
| FileMetadata | `SwordFsInode` (namespace, link count, attributes and EOF) and `ChunkIndex -> ChunkID` | `IMetaEngine` / RedisMetaTxn |
| COW ChunkMetadata | `ChunkID -> COWChunkHead{COWChunkRevision,size}` and revision allocator | `COWChunkMetadata` |
| Object data | Immutable bytes named by `(ChunkID,COWChunkRevision)` | `IDataEngine` |
| Runtime | Dirty buffers, local chunk sessions and per-inode operation lock | `FileReadWriter`, `FileChunkManager`, `COWChunk` |

`ChunkIndex` is a checked 64-bit logical coordinate derived from file offset
and the persisted volume chunk size. `ChunkID` is volume-scoped, nonzero,
monotonically allocated, and **never reused** after detach. COW revisions are
strictly scoped to one `ChunkID`. A revision number has no meaning without its
ChunkID; it is never stored in FileMetadata. The physical object key is derived
from `(ChunkID,COWChunkRevision)` by `COWObjectKey`, not from inode/index.

The logical mapping does not contain COW size, revision, buffer state or
object-key strings. The private COW head contains revision and size and is
updated by full-head compare-and-swap. The only *logical* visibility decision
is the FileMetadata attachment and EOF; the private head chooses the latest
immutable contents for an attached ChunkID. A newer revision supersedes the
previous one for **all** reads; it does not create a snapshot-read contract.

Mount composition loads one volume-fixed `ChunkType`, opens the corresponding
typed `ChunkMetadata` capability and constructs its `ChunkFactory`. The COW
mechanism is not accessed through Redis's transaction objects. The sole shared
COW-side helper retained by FileMetadata is a pure `ChunkCleanupWorkEncoder`
with `EncodeDetachedDelete` and `EncodeDetachedReclaim`. It serializes detached
ChunkID identities without reading or mutating COW metadata or object storage.

## Reads and sparse files

1. `FileReadWriter` takes the per-inode operation lock and obtains
   `ReadFileChunkSnapshot(ino,index)` from FileMetadata.
2. A missing attached ChunkID within EOF is a hole (zero-filled). The common
   inode size is the only visible EOF boundary.
3. An attached ChunkID resolves a COW-private head and reads the immutable
   object `(ChunkID,revision)`. Its visible byte limit is the intersection of
   the COW head size, chunk bounds and FileMetadata EOF.
4. A concurrent head advance invalidates an in-progress stale read; retry
   against the current head. If GC removed an obsolete object between the
   head lookup and GET, retry the new head rather than exposing stale data.

`ReadFileMappingSnapshot` is the authoritative whole-file input to size
planning and logical reclaim. `ProbeAttachment` tests FileMetadata
reachability; neither accepts a private COW revision as authority.

## Write, flush and publication

For a newly allocated logical chunk:

1. Create a dirty local buffer. Its bytes are not remotely visible.
2. Allocate a fresh ChunkID and one COW-private revision; put immutable object
   bytes before publishing the private head.
3. CAS the COW head for the new ChunkID; do not infer success from an upload
   acknowledgement alone.
4. Atomically `AttachPrepared(ino,index,ChunkID,end,expected)` in FileMetadata,
   validating the current EOF and retained boundary. Successful attachment
   makes the ChunkID reachable. A known losing attachment remains unreachable
   and is eligible only for safe best-effort cleanup.

For rewriting an already attached ChunkID:

1. Read its current COW head and prepare a COW write buffer; concurrent
   incoming writes must not mutate the immutable buffer being flushed.
2. Allocate a new *per-ChunkID* revision and upload the new immutable object.
3. CAS `COWChunkHead{revision,size}` against the expected head, then
   `FinalizeAttachedWrite` against the unchanged attached ChunkID and expected
   FileMetadata EOF/boundary. Retry conflicts from fresh authoritative state.
4. Once success is known, enqueue only the superseded private revision for
   best-effort cleanup. An error registering cleanup cannot roll back a
   successfully published head.

Only the latest published revision serves reads after a rewrite. Flush may
keep its published buffer to satisfy subsequent reads; closing a chunk
session releases the buffer. No mutable snapshot semantics or cross-mount
coherence lease is promised.

The two metadata domains are **not** one atomic transaction. Successful CAS
of a COW head does not imply FileMetadata attachment/EOF was committed, and
vice versa. Every retry must reread authoritative state and must distinguish a
known rejection, a known success and an outcome whose acknowledgement is
unknown. Do not infer that a Redis timeout means a command was not applied.

## EOF transitions

All file-size mutations go through the `FileReadWriter` operation lock and
boundary sanitation. The common `SetAttr(kSize)` and direct common truncate
paths are deliberately rejected: they would bypass the private COW head
clamp and attachment coordination.

`PlanChunkSizeChange` derives a `ChunkSizePlan` from a consistent snapshot of
inode EOF, boundary ChunkID and all mappings. `CommitShrink` or `CommitGrow`
revalidates the precondition under the Redis optimistic transaction and
atomically updates inode EOF and FileMetadata mappings. A stale snapshot
returns a conflict; recompute from current state rather than replaying stale
attached identities.

Shrink detaches whole mapped chunks beyond new EOF and clamps the retained
boundary COW head without minting an invented revision. A truncate/grow
sequence must not resurrect older data beyond EOF. Grow only changes logical
EOF and must leave new regions zero-filled until explicitly written. Concurrent
write/flush and truncate are ordered by the inode operation lock; remote
changes are rejected/retried using authoritative EOF/attachment checks.

## Reclaim, pending deletes and crash safety

Last-link deletion creates an orphan marker while the live inode still exists.
Open handles delay reclaim; after the local open fence clears,
`PrepareReclaim` checks `nlink == 0`, reads and WATCHes the entire typed
FileMetadata mapping, freezes `ChunkID` identities through the selected pure
encoder, then deletes the live inode/mapping and queues opaque `ReclaimWork`.
A non-regular inode freezes an empty-but-valid private payload. The inode
removal is the point of no return for namespace authority. Retry of a
partially acknowledged transaction must derive its outcome from the current
inode, not treat an old cleanup record as live authority.

Shrink and other typed detaches may queue `PendingDelete`; private COW
revision replacements queue typed revision candidates. Both records are
**maintenance hints**, not permission to delete. Omission or duplicate
registration can leak temporary storage but cannot affect logical reads.

The private chunk GC worker must perform a fresh authoritative reachability
check before deleting any object:

- Detached ChunkID: if FileMetadata still references the ChunkID, never delete
  its head or immutable objects.
- Private revision: if the current COW head still names that revision, never
  delete that object; a newer head is not a snapshot retaining old bytes.
- Reclaim work: revalidate all referenced chunks before deleting any, so a
  failed lookup cannot partially delete an apparently safe collection.

Errors from FileMetadata probing, COW head reads, malformed typed cleanup
payloads, or backend deletes fail closed and retain work for retry. The
persistent worker may replay work after a crash and must acknowledge only
completed items; no VFS cache or mount-local waking is a prerequisite for
safety. An already-reclaimed inode's maintenance record is not evidence of a
live namespace object.

Opaque `ReclaimWork` and `PendingDelete` envelope codecs are common only in
serialization shape. Their payload uses the COW-private `kCowTypedCleanup`
record kind; the selected cleanup participant owns decoding and reachability
checks. No common descriptor decoder or backwards-compatibility record path
remains.

## Verification boundaries

Coverage is expected for typed ID attachment/rewrite, partial/ambiguous Redis
EXEC, EOF conflict and retained-boundary handling, empty/sparse reclaim, 64-bit
chunk-index serialization, queue retries, malformed work, GC reachability,
last-open orphan fences, crash recovery and unpublished-object cleanup. The
end-to-end POSIX compatibility suites are additional checks, not a substitute
for per-file production coverage and correctness regression tests.

See [Redis schema](redis-metadata-schema.md), [data structures](data-structures.md),
and [architecture](architecture.md).
