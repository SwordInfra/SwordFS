# #382: metadata-only `copy_file_range` using shared Slice references

Status: **revised design review, not implementation-ready** (2026-10-19).
Owner: [#382](https://github.com/SwordInfra/SwordFS/issues/382) under [#255](https://github.com/SwordInfra/SwordFS/issues/255).
Review surface: [Draft PR #499](https://github.com/SwordInfra/SwordFS/pull/499).

## 1. Product capability: reference-copy, not a byte-copy implementation

Implement Linux `copy_file_range(2)` so that copying an already-published
file range creates **destination metadata references to immutable source Slice
objects**, rather than downloading the source bytes and uploading duplicate
objects. Both files can later be overwritten independently (redirect-on-write),
without changing the shared immutable object contents.

A user can already copy files using ordinary `read`/`write`; today the
`src/fuse/Vfs.cpp::SwordFsCopyFileRange` callback unconditionally replies
`ENOSYS`. The **new** capability is native range-copy with metadata-only
physical-data reuse, including partially aligned and sparse ranges. This is
analogous to JuiceFS's filesystem-side range clone, not ordinary userspace
copy hidden in the daemon. A common utility such as `cp` *may* use the syscall,
but the supported contract is the syscall itself, not a promise about every
utility's fallback behavior.

**Expected economics:** For an already-published 1 GiB source, a metadata copy
requires **no physical 1 GiB object GET/PUT**; its work is proportional to
the number of source Slice spans, destination chunk boundaries, required
metadata changes and their transaction/retry costs. It is **not O(1)** for
arbitrarily fragmented files. Source/destination dirty data may first need
normal Flush I/O. No zero-Redis-RTT, zero-CPU, zero-memory, object-store server
copy, cross-volume copy, reflink API, or atomic whole-file snapshot claim.

## 2. Required architecture and dependencies

SwordFS's common FileMetadata exclusively owns inode EOF/attrs and
`ChunkIndex -> ChunkID`. The Slice mechanism owns the ordered private
`ChunkID -> SliceRecord[]`, where each record has
`(pos, SliceID, size, off, len)` and later committed records overlay earlier
ones. An immutable Slice object is named by volume-scoped
`slice/<SliceID>` **independently of ChunkID or inode**. The existing codec and
`ResolveRange` already support clipped subranges and zero records.

A destination reference can therefore reuse the **same SliceID**, adjusting
only its destination logical `pos` and source object `off`/`len`, splitting at
logical destination chunk boundaries as needed. No new physical `Put` is
required for bytes already materialized in immutable source Slices. A source
hole is **explicitly copied as zeros**, including over previously populated
destination bytes; skipping an unmapped source hole would be incorrect.

Dependencies and status:

| Work | Why #382 needs it |
| --- | --- |
| #270 / #483 / #484 | **COMPLETE:** private Slice index, identity and ordered range resolver |
| #485 / #486 | **COMPLETE:** immutable Slice writer and range reader; local dirty visibility |
| #424 / #271 | **OPEN:** proven authoritative publish/EOF/ChunkID ordering, ambiguous-outcome recovery and committed Flush; copy must reuse that contract |
| #272 | **OPEN:** truncate, zero-cover and shrink/grow correctness across copies |
| #273 | **OPEN:** reachability-safe object reclamation; #382 additionally needs references shared across **different ChunkIDs/inodes** |
| #274 / #275 | **OPEN:** bound metadata/read fan-out and activate verified Slice volumes before claiming the public feature |

**Consequence:** #382 implementation is **deferred until the Slice publication
and cleanup foundations exist**. The draft document can be reviewed now, but
there is no accepted metadata-only implementation path on today's **COW-only**
selectable volume. Transitional COW object identity uses `(ChunkID,revision)`
and has no safe, multi-Chunk shared-object reference lifecycle. Do not bolt
sharing onto temporary COW, enable the syscall with a Read/Write loop, or
pretend unfinished Slice Flush is durable. While unsupported, keep the FUSE
callback's existing behavior; `generic/434` remains `known_unsupported`.

**Dependency ownership:** #424/#271 must solve authoritative Slice
publication **for ordinary writes first**. #273 must define a cleanup
extension point compatible with future shared Slice references; #382 owns the
extra shared-reference acquisition/lifetime integration and its own GC tests.
Do **not** automatically expand #275's activation scope or hold its merge for
#382. This is a separate feature on top of a sound Slice engine.

## 3. Intended metadata-copy path

1. FUSE callback uses `RunFuseInFiber`, copies input `fi` values by value,
   resolves and validates both live handles, access modes and file type.
   `flags` must be zero; reject invalid/negative/overflowing offsets,
   destination `O_APPEND`, immutable/append-only restrictions and overlapping
   source/target ranges when `source inode == destination inode` (even when
   descriptors differ). Preserve kernel-owned descriptor offset handling.
2. **Quiesce/Flush** accepted mount-local dirty writes on source and
   destination through their existing coherent Flush boundary **before**
   constructing reference metadata. This ensures the source bytes being
   shared exist as immutable published Slice objects and prevents old
   destination dirty overlays from later hiding the copy. If either Flush
   cannot establish a confirmed authoritative state, fail **without claiming
   any copied bytes**. Do not read-and-upload dirty bytes as a fallback. Flush
   may itself perform object PUTs for the *previously dirty* ranges.
3. For each bounded range, take an **authoritatively valid source view**:
   source EOF, current ChunkID attachments and the resolved **published**
   Slice spans. Clip at EOF and translate source-to-destination coordinates.
   For each data span, use the same `(SliceID,size,off)` with a new destination
   `pos,len`; for holes/zero covers, emit destination zero records so older
   destination data cannot reappear. An empty portion beyond source EOF
   causes a short/zero return, not destination extension.
4. **Protect shared physical reachability before publishing** destination
   references (see §4). A source truncate/delete/compaction race must not
   allow GC to delete the object between observing the source record and
   securing the destination's reference. Revalidate changed attachment/
   source metadata as required by #424; bounded conflict retries or error,
   never a fabricated success. Staging protection that is not published may
   leak only as explicitly permitted by best-effort maintenance.
5. Append/cut over the destination's private Slice records under the **#424
   authority/linearization protocol**, validating its currently attached
   ChunkID/EOF and respecting truncate/grow/overlap from other mounts. A new
   destination ChunkID is prepared/attached using #271's valid first-record
   protocol; never create an attached but missing/empty private index. Update
   destination EOF, mtime/ctime and publish-related cache invalidation through
   the established path. The first-stage atomicity boundary is **one bounded
   published chunk-range group**, not the entire file; failed later groups
   may return the already confirmed prefix.
6. A group counts toward `copied` **only after its metadata visibility and
   shared reference lifetime are confirmed**. Redis unknown outcomes require
   read-back/proof with an operation identity; never replay raw non-idempotent
   `RPUSHX` blindly, release a potentially live Slice or claim copied bytes on
   ambiguous publication. On failure with a confirmed prefix, return the
   prefix; otherwise return errno. Use one FUSE reply; failed delivery does
   not roll back already-authoritative committed data.
7. After successful reference publication, reconcile same-mount destination
   cached Slice views/EOF/dirty state; remote readers follow the existing
   publication/coherency contract. Later destination writes create fresh
   SliceIDs and do **not** mutate shared objects.

**Bounded work:** Cap the number of Slice records/metadata bytes staged in a
single operation and the number of destination chunk groups per FUSE request;
allow short positive returns. Limits will be chosen from measured costs
when Slice publishing and GC are operational. A fixed `1 MiB` data buffer and
`8 MiB` data copy budget from the old design are **removed**: the important
cost metric is **metadata fragmentation**, not logical byte count.

## 4. Physical-object lifetime: the essential new safety rule

**Invariant:** An immutable `SliceID` object must remain readable while any
*authoritatively reachable* Slice index, possibly in a different inode or
ChunkID, references it; cleanup cannot infer unreachability by checking only
its original source ChunkID. An old source object must survive source
truncate, overwrite, unlink/reclaim, compaction, source-mount unmount and
process restart until all destination references have become unreachable.

The existing #273 single-private-index reachability design is **insufficient
by itself** once #382 shares a SliceID across indices. Before implementing
#382, prove a **Slice-private shared-reference protocol** with at least:

- A durable, concurrent-safe reference acquisition/pin before destination
  publication, serialized against physical GC's decision to delete. A
  detached/compacted source must not let an already deleted object be newly
  referenced. Lease expiration without fencing cannot be used as the proof.
- Commit/abort/ambiguous-outcome behavior for prepared versus published
  references. Conservatively retained orphan pins after faults are allowed;
  **leaking on an exceptional path is safer than deleting live data**.
- Safe release on destination overwrite/truncate/unlink and compaction, with
  no early decrement while an index record still authoritatively references
  the object. Zero spans require no physical reference.
- Object deletion only after revalidating *global SliceID reachability* (or
  an equivalently authoritative shared reference count and any in-flight
  readers/pins), not a full-volume scan or best-effort approximate counter.
- Same-volume-only sharing and a persistent reference identity. SliceIDs
  never turn into common FileMetadata fields or a COW revision concept.

**Candidate, not preapproved implementation:** a Slice-private Redis ref/pin
record with a small watched acquire-versus-GC state transition and idempotent
copy operation identity, plus indexed reference retirement. This must be
compared with JuiceFS's `sliceRefs` accounting and with #273's actual cleanup
protocol **after #273 is implemented**. The complete consistent lifecycle,
including source-detach/GC races, is a **design gate**, not something the
existing `RedisSliceChunkMetadata::AppendRecord` already guarantees.

Avoid introducing a generic distributed transaction manager, global
file-locking, a copy of immutable object bytes, or a new reference format in
mechanism-neutral FileMetadata to solve this problem.

## 5. Concurrency and observable semantics

- Source snapshot is a consistent published range at an identified metadata
  authority point, not an instantaneous snapshot across the whole file.
  Concurrent source writes/truncate and destination modifications are handled
  through #424's conflict validation/cutover; bounded metadata-copy groups
  may observe different committed source states. Same-inode non-overlapping
  copies must snapshot/protect their source references before destination
  updates can replace them.
- Data already copied remains valid even if the source subsequently unlinks,
  overwrites or truncates. Destination range overwrites obey last-committed
  SliceRecord precedence and hole zero-cover semantics. No app-visible
  file-wide atomicity promise or mandatory application advisory locks.
- Failed/revoked metadata authority does not warrant returning a fabricated
  byte count, re-uploading physical bytes, corrupting old references or
  automatically unmounting FUSE.
- `copy_file_range(2)` handles source/destination offset-pointer changes in
  the Linux syscall layer; SwordFS's FUSE callback receives explicit offsets
  and returns a copied count. Source EOF/short-copy behavior, flags and file
  types follow the supported Linux ABI.

## 6. JuiceFS peer review and deliberate SwordFS differences

Actual JuiceFS code reviewed (upstream main, 2026-10-19):

- `pkg/vfs/vfs.go::CopyFileRange` validates handles, offsets/flags and
  overlap; serializes affected handles; **flushes both source and
  destination**; delegates to `Meta.CopyFileRange`; invalidates destination
  writer/reader caches and attributes.
- `pkg/meta/redis.go::CopyFileRange` reads the source's committed Slice
  records, maps covered subranges into destination records without object
  data-copy, updates destination EOF/mtime/ctime and increments shared Slice
  reference counts (`sliceRefs`) in its Redis transaction. Its physical
  blocks remain immutable and shared; later overwrites use new data.
- JuiceFS docs explicitly describe metadata cloning and `cp` over Linux
  `copy_file_range` on supported kernels.

**Adopt:** metadata reuse, source/destination Flush boundary, subrange Slice
references, hole semantics, destination cache invalidation and safe shared
object lifetime. **Adapt:** SwordFS keeps `FileMetadata` and mechanism-private
Slice Metadata separate and uses #424's narrowly scoped authority protocol;
JuiceFS's combined metadata transaction must **not** be assumed available or
copied without a proof for SwordFS's split publication model. **Do not adopt:**
whole-object physical copies or a forced generic refcount in common metadata.

References:
- https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs.go
- https://github.com/juicedata/juicefs/blob/main/pkg/meta/redis.go
- https://juicefs.com/docs/community/guide/clone/
- https://man7.org/linux/man-pages/man2/copy_file_range.2.html
- [#424 global Slice ordering](https://github.com/SwordInfra/SwordFS/issues/424)
- [#273 Slice cleanup](https://github.com/SwordInfra/SwordFS/issues/273)

## 7. Mandatory test-first contract after design dependencies close

Before any production implementation, establish **real CI RED** for
`copy_file_range` while FUSE still replies `ENOSYS`; promote to GREEN with
metadata-only reference implementation. No actual RED/GREEN is claimed by
this design PR. Required tests include:

1. Full and partial cross-file copies; range offsets, sub-Slice offsets,
   cross-Chunk splits; dirty source/destination Flush boundary; explicit
   zero overwrite for sparse source spans; destination timestamps/EOF;
   post-copy overwrite of either file does not mutate the other.
2. Same-inode nonoverlap allowed and overlap rejected (including hardlink
   alias); bad handles, read/write access, flags, `O_APPEND`, inode policies,
   bad sizes, EOF and short-copy; correct FUSE byte-count reply.
3. Source truncate/unlink/compaction/GC and destination remount after
   copying: destination reads **identical bytes** without a source object
   re-upload. Simultaneous source detach and copy acquisition in both
   schedules; no prematurely deleted shared object; long-lived shared
   references release after all users detach.
4. Two-Mount overlapping write/truncate/grow/zero-cover and copy races;
   unknown/lost Redis replies during reference acquisition and destination
   publication; confirmed-prefix partial result and read-back proof;
   duplicate retry does not duplicate references or lose objects.
5. Instrument `IDataEngine` on clean-source copy: assert **zero data-object
   GET/PUT for the copied range**, including large copies; measure metadata
   commands/bytes versus source fragmentation and destination chunk count;
   bound per-call metadata memory/work and read fan-out.
6. Real FUSE syscall E2E (no application fallback) and pinned fstests
   `generic/434`. Only after authoritative PASS, promote `generic/434` from
   `known-gaps.tsv` to `supported.txt`, update #255 and design docs; do not
   alter the independent `generic/564` environment classification.

Every changed production C/C++ file must meet **>90% per-file Codecov patch
coverage** plus full CI and independent review. The implementation must
not be advertised on COW-only volumes by silently falling back to read/write.

**Design disposition:** user-selected metadata-only direction accepted.
The major unresolved implementation design gate is durable shared SliceID
lifetime, including the precise acquire-versus-GC protocol. #382 is **not**
implementation-ready until the referenced Slice publication/GC dependencies
and that gate are resolved. This is a deliberate deferral, not a claim that
current Slice code supports copy today.
