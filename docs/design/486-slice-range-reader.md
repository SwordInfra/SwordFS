# Slice Chunk exact-range Reader (#486)

Status: **Reader and local sparse overlay implemented; reviewer F1/F2 corrections under verification; stacked integration pending**.
Canonical issue: [#486](https://github.com/SwordInfra/SwordFS/issues/486).
This work is a **stacked Draft PR** on
`feat/484-redis-slice-index` (PR #488), which itself is based on
`feat/483-slice-record-overlay` (PR #487).

## Purpose and non-negotiable invariants

### Retained-boundary provenance (review #491)

An unversioned, session-local upper bound must not hide newly authoritative
data forever. A retained boundary is scoped to the old attached ChunkID and
the ordered-record history present at sanitation time: only those historical
records are clipped to the surviving prefix. Later committed records on that
same ID can cover the old hidden suffix. A subsequent attachment of a new
ChunkID invalidates the old boundary. Growing EOF without a new covering
record must not resurrect the truncated historical suffix. A failure to
identify the boundary's authoritative source must fail closed.

The reader can enforce this local visibility model, but full concurrent
shrink/grow/zero-cover publication and remount persistence remain #272/#424
acceptance criteria; activating Slice volumes remains #275's responsibility.
This PR does not claim to implement the publication protocol.
The retained boundary is captured using the current FileMetadata mapping and
its Slice record count. The capture is made outside the fiber mutex, and a
failed capture makes reads fail closed because the common boundary callback
cannot return a `Status`. A later successful sanitation capture can restore
the view. When reading, historical records at/after the old visible prefix
are hidden (overlapping records are clipped), while subsequently appended
records and new attachments remain authoritative. The full stored history is
validated before such historical clipping; a shorter history under the same
ChunkID fails as metadata corruption.

Repeated sanitation must preserve **each event's temporal provenance**. For
example, clamp the first record R0 to `[0,4)`, append R1 at `[4,8)`, then
clamp the now-two-record history to `[0,6)`: R0 remains limited to `[0,4)`
but R1 retains `[4,6)`. A later metadata-only grow must not expose the old
`[6,8)` suffix. A single `min(previous_prefix, new_prefix)` applied to
both R0 and R1 is incorrect. The mount-local view therefore maintains a
monotonic sequence of `(record_count, visible_prefix)` sanitation epochs for
one attached ChunkID. An epoch restricts only records with index below its
captured count; the read projection uses the most restrictive subsequent
epoch for each record. Same-history clamps coalesce by minimum prefix, but
clamps after additional records may not collapse into older epochs. A new
attachment clears stale provenance. Projection is linear in records plus
epochs, not their product.

The narrow #486 tests cover fresh and cached logical Chunk reads plus EOF
clipping within `Chunk::Read`. Mounted `FileReadWriter` fanout across multiple
Chunk boundaries, multi-mount concurrent truncate/grow, and true remount
sanitation persistency cannot be exercised on a Slice volume until activation
and publication exist; treat these as mandatory #275/#272/#424 integration
gates rather than claiming that #486 independently proves them.

### Implemented reader boundary

`ChunkFactory` selects `slice::SliceChunk` only when passed a real typed
`SliceChunkMetadata` capability. `SliceChunk::Read` refreshes the current
FileMetadata attachment, fetches one complete private ordered record history,
calls the production `ResolveRange` on the visible requested window, and issues
only exact range `IDataEngine::Get` calls for nonzero spans. All data is staged
in a request-sized buffer; the caller's output is appended only after every
source succeeds. A mapped ChunkID missing a nonempty index fails closed.

`SliceChunk::Write` maintains nonoverlapping sorted accepted dirty intervals;
only intersections of those intervals are copied into a read's local snapshot,
and local overwrites replace previous accepted bytes without hydrating the
published object. The read planner subtracts accepted Dirty Range coverage
before physical GETs, so already-overridden bytes consume no remote bandwidth.
`TruncateLocal` clips the dirty view, whereas `Flush` is
intentionally `NotSupported` until the writer and publication milestones are
implemented. The common `Chunk`/`IMetaEngine`/`IDataEngine` interfaces stay
unchanged. The physical key is `slice/<decimal-SliceID>`.

Read a small byte range from a file backed by an ordered Slice Chunk without
hydrating a full logical Chunk or leaking Slice details into generic VFS,
FileMetadata, or `IDataEngine`.

1. **Reachability:** FileMetadata owns the mapping
   `(inode, ChunkIndex) -> optional ChunkID`. If there is **no** mapping, the
   published view is a hole (all zeroes). If a mapping exists, its Slice-private
   index MUST contain at least one record. A missing, empty or malformed
   index is a hard error, never a zero-filled read. This reflects the agreed
   #484 creation sequence: prepare object + first record **before** attachment.
2. **Visibility:** For mapped ChunkIDs, load the ordered private history through
   `metadata::slice::SliceChunkMetadata::ReadRecords` (#484) and resolve just
   the requested logical range through `chunk::slice::ResolveRange` (#483).
   Later *committed* records win per overlapping byte. ID numerics and
   physical-object creation times do not define LWW order.
3. **Exact bytes:** A zero-cover record and unrecorded gaps yield zero bytes.
   Nonzero `SliceID` records require an exact object-range read
   `[ResolvedSlice.off, off + len)`. Missing backing objects, short reads,
   failed GETs and corrupted metadata are **not holes**. A successful
   `Chunk::Read(offset,len)` appends exactly `len` bytes to the destination.
   On failure, do not leave partially appended data in the caller's buffer.
4. **Local read-your-writes:** Accepted dirty bytes, whether in the writable
   generation or an in-flight frozen Flush generation, override committed
   remote bytes. For overlapping local generations, the *newest accepted
   write* wins. A successful or failed Flush must not hide writes accepted
   during that Flush. A sparse dirty interval does not make unrelated parts
   of a 64 MiB Chunk dirty, and a dirty session must not force remote
   hydration during `Write`.
5. **Published EOF:** `FileReadWriter` clips the overall read to its visible
   file EOF and splits at logical Chunk boundaries. The Slice Chunk reader
   respects the per-Chunk request bounds, and it must not expose bytes past
   a proven file-size boundary or infer committed EOF from SliceRecord.size.
6. **No premature activation:** #486 adds a real production
   `Chunk::Read -> ResolveRange` call path through the mount-selected
   `ChunkFactory`, not a fake dead-code-audit call. It does **not** enable
   formatting/mounting a Slice volume before #275, because Slice cleanup,
   physical Append and final metadata publication are not yet implemented.

## Data ownership and API placement

The existing VFS shape is already suitable:

```text
FileReadWriter::Read (file EOF clipping, cross-Chunk fanout)
  -> FileChunkManager::Get (cache a logical Chunk session)
    -> ChunkFactory::Open (select COW or Slice)
      -> chunk::slice::SliceChunk::Read (mechanism-specific read view)
          -> IMetaEngine::ReadFileChunkSnapshot (attachment / EOF)
          -> SliceChunkMetadata::ReadRecords (ordered private history)
          -> ResolveRange (#483, range projection)
          -> IDataEngine::Get (one or more exact physical subranges)
          -> overlay accepted local dirty intervals
```

The ChunkFactory gets a typed `SliceChunkMetadata` capability exactly as it
already does for COW metadata, without adding Slice fields to `IMetaEngine`.
The Slice implementation owns a logical per-Chunk session with a per-fiber
synchronization boundary using existing `utils::FiberRWMutex`. A cached
session cannot assume its originally observed ChunkID is still authoritative:
its clean published read must refresh the attachment and ordered private
history as needed; missing private state for a *currently mapped* ID is fatal.
Do not cache permanent "not found" decisions across other mounts' writes.
Bounded retry of a *proven changed attachment* may be added if tests prove
it necessary; do not hide an S3 failure by arbitrarily retrying.

The new reader must not copy COW's `WriteBuf` model: that buffer represents
an entire hydrated prefix. Instead expose or internally own a **sparse dirty
interval layer** retaining accepted data with source offsets. Its ordinary
production consumer is `SliceChunk::Read` and ordinary producer is
`SliceChunk::Write`; **no test-only setters, production hooks, or fake
insert-record APIs**. In #486 `Write` may stage accepted data locally and
make it immediately readable, but `Flush` must return a clear NotSupported
until the real #485 Writer/#271 publisher exists. Do not silently report a
durable Flush or drop buffered writes. #485 extends the same state ownership
with immutable frozen generations and submission/acknowledgment ordering.

Tests may seed *already published* historical SliceRecords through the real
#484 typed metadata API and seed matching bytes with `IDataEngine::Put`.
An integration fixture may attach the initial ChunkID through the existing
FileMetadata interface, preserving the agreed first-record-before-attach
order. This is legitimate production configuration, not a test-only
production interface.

## Physical object key: one agreed identity for #485 and #486

Add one Slice-private deterministic `SliceObjectKey(SliceID)` formatter,
shared by the reader and future writer, selecting exactly
`slice/<decimal-SliceID>` **relative to the volume's data-engine prefix**.
No new key is persisted in Redis: `SliceID` is volume-scoped and
already recorded by #484. It must not collide with COW's
`<ChunkID>/<revision>` namespace. Do not include the logical `pos`,
`ChunkIndex`, historical `size` or mutable Flush generation in the object
identity, since the *same* SliceID may grow after Flush. #485 must use the
identical helper for initial `Put` and later `IDataEngine::Append`, and
#273 for eventual reclamation. A zero SliceID is a zero cover and never a GET.

Current `IDataEngine::Get(key,offset,size,out)` is sufficient for bounded
range fetches. #486 must not change the data engine to support new object
operations; native `Append` is #485. Object-store backends that do not
support native append still support reading already published objects, but
Slice activation remains prohibited until writer capability is validated.

## Read algorithm and failures

1. Validate output pointer, target tailroom, `offset <= chunk_size`, and
   `len <= chunk_size-offset`. Keep the original output length unchanged on
   failure, including when the third physical span fails.
2. Determine the currently attached ChunkID through a FileMetadata snapshot.
   Unattached -> a synthetic all-zero published view; attached -> read
   `ReadRecords` once. If missing/empty, return integrity error.
3. Call **the real #483 `ResolveRange`** on the complete history and the
   requested range. The resolver validates *all* records, including those
   outside the requested subrange, and returns ordered, coalesced spans.
4. For every nonzero span, derive the `SliceObjectKey` and do an exact
   `IDataEngine::Get` at the span's physical offset/length. Verify the
   exact number of appended bytes. Zero spans are materialized locally.
   Work only on the requested range, not the entire Chunk; perform bounded
   reads with the existing backend worker/concurrency model.
5. Overlay dirty generations newest-first **in accepted-write order** on the
   successful published view. A dirty write replaces only intersecting
   bytes; gaps preserve published bytes or zeroes. Snapshot immutable
   generation ownership under the fiber mutex so concurrent Write/Flush
   cannot race buffer lifetime; never hold a global unrelated lock across
   arbitrary S3 operations. Finalize the target output only after success.

### Read visibility versus physical append

SliceID is stable across eligible physical append; its older record's
`size` is a historical upper bound for referenced offsets, not a request
to hydrate all bytes or an instruction to consult current object length.
Read exactly each *published* span. #485 must guarantee that append preserves
all previously acknowledged prefix bytes. Do not assume an object is
immutable simply because COW objects were immutable.

FileMetadata snapshot and Slice-private LIST are deliberately separate
authorities. #486 must not claim this is one atomic global snapshot or
smuggle in a combined Redis transaction. The globally authoritative
commit/attach/truncate/overlap sequencing remains #424/#271. Record
observable stale-remap races and fail closed when definitive. A short S3
read or an unavailable index does **not** become zero data by retry.

## Peer study: JuiceFS

JuiceFS `pkg/vfs/reader.go` reads Slice spans after obtaining a Chunk's
ordered metadata and requests only the needed backing-object byte ranges.
Its `dataReader.readSlice` uses the record's physical `Off` and returns
errors/short-read failures instead of silently filling real Slice data with
zeroes. This is the semantic lesson to **adopt**.

**Adapt:** SwordFS uses typed mechanism-private ChunkMetadata, direct
`IDataEngine::Get`, and a common `Chunk::Read` interface rather than
JuiceFS's reader-cache/goroutine layer. Reuse #483 `ResolveRange`,
`FileReadWriter` fanout and the existing S3 BlockingExecutor.
**Do not adopt yet:** JuiceFS read-ahead/page cache, speculative fetching,
new worker pools, cache invalidation machinery and whole-block hydration.
Those are separate measured optimizations, not requirements for #486.

## Explicit alternatives / trade-offs

| Alternative | Decision |
|---|---|
| VFS directly reads Slice metadata and object ranges | Reject: leaks mechanism-private state across `Chunk` abstraction |
| Load every record and hydrate the entire logical Chunk | Reject: 4 KiB cold read could download 64 MiB |
| One `SliceChunk` Reader behind `ChunkFactory` | Adopt: real production read path with narrow mechanism-specific logic |
| Reuse COW's full-prefix `WriteBuf` | Reject: defeats sparse local overwrite and Dirty Range semantics |
| New generic storage/version abstraction for each SliceRecord | Reject: `SliceID` plus stable physical key is sufficient for this beta design |
| Activate Slice mount/Flush now to satisfy dead-code audit | Reject: incomplete Writer/GC would falsely advertise a working filesystem |

## Test-first contract and verification gate

- **RED before C++ implementation:** add focused tests using the normal
  `ChunkFactory` and `Chunk::Read/Write` APIs, plus real FileMetadata and
  deterministic data-engine doubles. RED must show the missing Slice
  runtime selection/reader contract, not an unrelated infrastructure failure.
  Use GitHub focused Dev Build, never a local compile.
- **Published-only:** unmapped ChunkID emits zeroes; mapped index with one
  record, multiple overlaps, sparse gaps, explicit zero covers, same SliceID
  historical size, misaligned and clipped offsets, remount and changing
  authoritative attachment. Object byte/GET counters prove bounded reads.
- **Dirty:** sparse writes read back immediately over published bytes;
  adjacent and overlapping writes order correctly; untouched ranges remain
  sourced from published data/zero. Simulate the later #485 frozen-generation
  boundary through actual state transitions, not test-only hooks; do not
  require #486 to fake an acknowledged persistent Flush.
- **Errors:** malformed/out-of-range records, mapped missing index,
  missing object, short `IDataEngine::Get`, midstream GET failure,
  wrong buffer capacity, zero-length read, at/after EOF, and cross-Chunk
  reads. On failure the caller's buffer must not retain partial output.
- **CI:** per changed C/C++ production file Codecov patch coverage >90%;
  formal CI, Debug/Release, no COW behavior regression, no memory backend,
  and independent review. Until #486 is truly wired into the production
  Slice factory branch, a standalone direct test call is not sufficient
  evidence to clear #487's `ResolveRange` dead-code audit.

## Stacked merge discipline and release gate

```text
main
  <-- #487: feat/483-slice-record-overlay
        <-- #488: feat/484-redis-slice-index
              <-- #486: feat/486-slice-range-reader
```

Do not change #486's PR base to `main` or copy files onto another branch.
After #486 adds the **real** `Chunk::Read -> ResolveRange` production caller,
verify all changes and integrate #486 into #488's branch, then #488 into
#487's branch, rerun #487's CI **on its updated HEAD**, and merge #487
only when every gate passes. Continue Slice activation only under #275;
#424/#271 govern safe authoritative publication and #485 the Writer.
