# Redis Slice-private ordered index — design review (#484)

Status: **LWW index design agreed; implementation pending**. This document is owned by
#484, based on #483's Slice model. The implementation PR must be stacked directly
on #487 (`feat/483-slice-record-overlay`), not on `main`.

## Contract and boundaries

`FileMetadata` still owns `ChunkIndex -> ChunkID`, and the Slice mechanism alone
owns `ChunkID -> committed ordered SliceRecord history`. The two metadata domains
are independent; #484 **does not** introduce a transaction spanning them or
claim a globally linearizable file write. #424/#271 own authoritative
cross-mount publication; #485 owns `IDataEngine::Append` and the Slice writer;
#486 owns actual range reads. Slice volumes remain unselectable until #275.

The typed `metadata::slice::SliceChunkMetadata : ChunkMetadata` surface should
expose only domain operations, for example:

- `AllocateChunkID(ChunkID *)` from the shared base and
  `AllocateSliceID(SliceID *)` for new, volume-unique Slice identities;
- `CreateIndex(ChunkID)` to establish an *empty but authoritative* Slice index;
- `ReadRecords(ChunkID, vector<SliceRecord> *)` to retrieve a complete,
  committed ordered history without exposing Redis types;
- `AppendRecord(ChunkID, SliceRecord)` for **unconditional, ordered, atomic
  record append** (subject to input and index-identity validation). This is
  a mechanism-private primitive, **not** the file write/EOF commit.

Names are illustrative, not permission to expose `RedisKvTxn` or the physical
key scheme. A lost acknowledgement from a non-idempotent `RPUSH` is
`OutcomeUnknown`; the caller must not blindly repeat the same append or
infer from an unsuccessful response that the record is absent. #271 owns
reconciliation in the final publication protocol, rather than requiring a
history-length compare-and-swap in this index API. Do not introduce an
independent `ChunkID` allocator or copy COW's `revision`/`head` scheme into
Slice.

`SliceID` and records remain Slice-private. In particular, a SliceID denotes a
**contiguous appendable logical Slice**, and records with the same ID can have
different historical `size` values. No global same-ID size-equality check.
Once another Slice or zero cover overlaps its existing range, the old Slice
cannot be appended again; the **writer and final authoritative commit** enforce
this rule (#485/#271/#424), not this read/index primitive.

**Agreed SwordFS writer policy (2026-10-15):** follow JuiceFS's *local Slice
selection principle* (prefer an eligible writable Slice; allocate a new one
when no candidate can accept the write), with one intentional difference:
**Flush does not automatically freeze a logical Slice**. Flush freezes and
persists a *dirty generation*, not necessarily the SliceID. A subsequent
write may append the same SliceID across multiple successful Flush calls if
the logical Slice is not frozen by a newer overlapping Slice/zero cover,
the append offset is correct, and the backing store can safely append only
the new tail via the required `IDataEngine::Append` (#485). Neither a prior
successful Flush nor `size` changing between records is grounds to reject
reuse. The local writer can make a provisional eligibility decision; #271/#424
must check authoritative concurrent publication before accepting it.

## Redis representation

Use the existing `RedisKey::PrivateChunkIndex(ChunkType::kChunkSlice, suffix)`
namespace and the existing `RedisBackendContext`/`RedisMetaClient`/
`RedisKvTxn`/`BlockingExecutor` infrastructure. No new thread pool, Redis
client, generic FileMetadata Slice fields, or Memory backend.

**Index key:** one Redis **LIST** per `ChunkID`, with:

- element 0: a fixed header/magic identifying the current Slice-private codec
  version **and the owning ChunkID**. Verify identity when loading the index
  and before accepting append operations so moving one LIST's contents to
  another ChunkID fails closed. The header also distinguishes an initialized
  *empty* index from a missing key (Redis cannot retain an empty LIST);
- elements 1..N: committed Slice records, oldest first, one compact binary
  record per element in the `(u32 pos, u64 id, u32 size, u32 off, u32 len)`
  24-byte format in **little-endian** (the existing `BufEncoder`/`BufDecoder`
  use `writeLE`/`tryReadLE`). Require exact length, header identity and record
  validation before returning any data.

Versioning protects current-format integrity, **not** beta format migration or
dual-reading legacy records. A header mismatch, corrupted or wrong-type key,
malformed record, over-limit payload or invalid identity is a hard error
(`Malformed`/I/O error), never an implicit zero range.

`CreateIndex` is idempotent **only for an existing valid empty index**; it must
never overwrite an index containing committed records, reset history, or
silently replace corrupted keys. Distinguish:

1. An **unattached** ChunkID at the FileMetadata boundary: a hole, checked by
   #486, not by guessing from an empty Redis result;
2. An allocated/unattached but *not yet initialized* private index: not found,
   eligible for explicit initialization;
3. An initialized index containing only the header: a valid empty history;
4. An **attached** ChunkID with missing, wrong-type, or malformed private
   index: **error**; do not synthesize an empty list/zero data.

Use native Redis `RPUSH` to append data, and `LRANGE` for a snapshot read in
append order. Where possible use `RPUSHX` for an existing initialized LIST
to prevent silently recreating an index that has disappeared. Extend the
existing `RedisMetaClient` with narrow LIST operations only as needed, not
a second wrapper or a new transaction framework. A **single** Redis
`LRANGE` retrieves one ordered snapshot. Validate the typed header and
records, fail closed on missing/wrong-type/corrupt state, and ensure a missing
LIST is not silently recreated by append. The index-identity check must not
be mistaken for a history-length CAS. Avoid per-record Redis queries and
eager rewriting of the entire record history on append.

Alternatives considered: one growing STRING with `APPEND` can preserve order
but complicates a simple empty-index marker and record decoding;
Hash fields and numeric-ID-sorted structures do not inherently preserve the
**commit** order. Redis LIST matches JuiceFS's proven `RPUSH`/`LRANGE` model,
while the header adds SwordFS's missing-vs-empty and wrong-ChunkID safeguards.

**Agreed LWW record-append semantics:** validate the incoming record against
#483's chunk-size/bounds rules (reuse/extract its validator rather than
duplicating checks), then append one record atomically in Redis LIST order.
Readers rebuild the visible byte ranges from oldest to newest; the last
*committed* record covering a byte wins. Distinct, disjoint records never
invalidate each other just because another record was committed in between.
SliceID numerical order, client start time, and recorded `size` are not
precedence rules.

**No default WATCH + expected LIST-length CAS.** It would introduce unnecessary
conflicts for independent writes, while merely observing an unchanged list
length is not the actual rule deciding whether an old Slice is still safe to
extend. #484 does not implement an extra per-append history version, expected
record count, or bespoke global publication transaction.

The crucial separate scenario is append-after-overlay: if mount A prepares an
extension of existing SliceID X while mount B publishes another SliceID Y
overlapping X, A must not then publish a stale extension of X. The append
eligibility and FileMetadata/EOF publication protocol, including the
authoritative commit-time ordering under concurrency, belong to #424/#271.
They may need **specific** validation/fencing; that is not a mandate to add
the generic per-append LIST-count CAS back to #484. Redis failures after
issuing a non-idempotent `RPUSH` can have unknown outcomes; reconcile against
authoritative history before any retry, without treating LWW as permission
to duplicate a record.

The index may be created before `ChunkID` is attached to FileMetadata;
unreachable private state is tolerable until #273 cleanup. #484 does not
provide a public commit protocol or publish file size/EOF.

## JuiceFS peer review: actual Slice selection and commit flow (2026-10-15)

Inspected upstream `juicedata/juicefs` main at commit
`d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb`:

- [`pkg/vfs/writer.go#L164-L184`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/vfs/writer.go#L164-L184):
  the local `chunkWriter.findWritableSlice` searches its **active local**
  slices newest-first. A candidate must be unfrozen, and `pos` must be in
  `[slice.off + floor(slice.slen / blockSize)*blockSize,
  slice.off + slice.slen]`. Writes can replace bytes in the not-yet-uploaded
  trailing block, not only append exactly at `slen`. If no candidate qualifies,
  the caller creates a **new** Slice; physical overlap is not the only trigger.
- [`pkg/vfs/writer.go#L272-L284`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/vfs/writer.go#L272-L284):
  a new `sliceWriter` receives a new SliceID and joins the local chunk's
  pending queue. No metadata LIST scan or record-count CAS is involved in
  the *creation-versus-reuse* decision.
- [`pkg/vfs/writer.go#L186-L232`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/vfs/writer.go#L186-L232):
  the local `commitThread` waits for each slice to finish and commits the
  `sliceWriter` instances in creation order. `Flush()` freezes active writers
  and eventually removes them from the local chunk queue, so the next write
  creates a new SliceID, even if spatially adjacent to the old committed
  Slice. The implementation does not search committed Redis history to
  reopen a previously finished SliceID.
  **SwordFS deliberately differs**: a successful Flush does not close logical
  SliceID reuse if the logical/physical append contract still permits it.
- [`pkg/chunk/cached_store.go#L255-L278`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/chunk/cached_store.go#L255-L278)
  and [`#L476-L510`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/chunk/cached_store.go#L476-L510):
  completed blocks cannot be overwritten; `FlushTo` uploads complete blocks,
  and `Finish` completes the slice. The storage backend uses `Put` for
  finished block objects, rather than assuming provider-native S3
  `AppendObject`. This is a physical-layout choice, **not** a proof that
  SwordFS must adopt exactly the same layout or forbid its requested
  `IDataEngine::Append` capability.
- [`pkg/meta/redis.go#L3140-L3183`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/meta/redis.go#L3140-L3183)
  and [`#L1144-L1190`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/meta/redis.go#L1144-L1190):
  the **metadata commit**, unlike local Slice selection, does use WATCH,
  specifically on the inode key. The same transaction executes `RPUSH` for
  the Chunk Slice LIST and `SET` for the inode EOF/attributes. It does **not**
  read or compare the LIST length as a Slice-creation precondition.

**Implication and chosen #484 design:** JuiceFS separates local writable
Slice reuse from metadata commit ordering. Its inode-key WATCH protects a
single combined Redis metadata domain, while SwordFS deliberately has
separate FileMetadata/ChunkMetadata authorities (#312/#424). **#484 chooses
plain atomic append of ordered records and interval LWW**, without LIST-length
CAS. Atomic record ordering alone does not prove globally correct Slice
publication or append eligibility: #424/#271 remain the proper design gate.
**SliceID reuse after Flush is allowed when eligible**; its writer algorithm
belongs to #485.

## Identity and allocation

Use a distinct volume-scoped Redis SliceID counter, not `NextChunkID` or COW
revision. Allocate with existing non-negative/overflow-safe Redis counter
mechanics (`RedisMetaClient::IncrNonNegative` or the narrowest required
extension). The identity is nonzero, at most signed 64-bit maximum, and never
intentionally reused. A lost allocation response can consume an ID without
returning it; **gaps are correct**. Do not retry an ambiguous allocation with
the assumption that the first allocation did not happen. Protect missing or
corrupt authoritative counter state from reset/reuse; define its initialization
at volume formatting, not by resetting it during mount. Because SwordFS is
beta, adjust the current format state directly, without migration adapters.

Physical S3 object append/identity is a **different** layer. The index does
not assert that every object store offers a particular physical `AppendObject`
API and does not implement `IDataEngine::Append` (#485).

## Error, performance and test strategy

All Slice metadata operations in business logic are fiber-domain entry points
and must call the existing blocking Redis executor. The Redis adapter itself
remains thread-domain. Preserve existing `Unavailable`, `OutcomeUnknown`,
`Malformed`, `InvalidArgument`, and known conflict semantics, including
partial EXEC reply faults; do not collapse ambiguous mutation into ordinary
failure/no-op.

Service-backed Redis tests must cover:

- allocation uniqueness across separate mounted typed instances and volumes,
  negative/invalid/exhausted counter, duplicate avoidance after ambiguous
  allocation (including an outcome that actually applied);
- index initialization, empty but present state, missing vs attached-missing,
  append order after reload/remount and per-ChunkID namespace isolation;
- same SliceID with growing historical sizes, zero records, clipping,
  overlap and exact `ResolveRange` byte-oracle reconstruction;
- codec roundtrips, wrong magic/length/type, truncation, invalid
  `pos+len`/`off+len`, overflow and read output unchanged on corruption;
- two-instance concurrent appends (including disjoint and overlapping
  records), deterministic LWW reconstruction, missing-index and wrong-header
  append failures, lost acknowledgement with applied/not-applied outcomes,
  no accidental double-append or silent mutation after error;
- correct fiber->POSIX worker transition and no new Redis thread pools.

**Complexity:** `RPUSH` is constant work per append (excluding input and
index-identity validation), and `LRANGE` loads the complete O(N) record history
into memory for resolution. Validate all records once per read rather than
issuing a Redis round trip for every record. Large-history bounds and
compaction remain #274; avoid pre-optimizing with a second eager resolved
representation in this Issue.

## Stacked PR workflow and actual merge gate

```text
main
  `-- #487  feat/483-slice-record-overlay (base: main)
        `-- #484  feat/484-redis-slice-index (base: feat/483-slice-record-overlay)
              `-- #486  real Slice Reader (base: #484 branch)
```

- #484's PR **must** target `feat/483-slice-record-overlay`; it must not
  accidentally target `main`. Its diff should include only #484 changes.
- A stacked PR **does not by itself repair #487 CI**: #487 is checked at its
  own HEAD, where `ResolveRange` has zero production references. Likewise,
  merely adding #484 storage APIs does not guarantee `dead-code-audit` passes.
- Once #486 actually invokes `ResolveRange` in a reachable runtime path,
  integrate bottom-up **into parent feature branches**: #486 -> #484 branch,
  then #484 branch -> #487 branch. Rerun #487's formal CI **on its updated
  HEAD**. Only merge #487 to `main` after its own gates, including
  `dead-code-audit`, pass. Do not suppress the audit or manufacture dummy
  invocations. After final integration, reconcile stacked PR targets and
  statuses/Issue closure without re-merging duplicate changes to `main`.

Until the real reader is ready, keep #487 and #484 Draft and do not claim that
their independent required checks can all pass simply because the branches
are stacked.
