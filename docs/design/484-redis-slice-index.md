# Redis Slice-private ordered index (#484)

Status: **design simplified by owner decision (2026-10-15), implementation and CI verification pending**.
The implementation remains Draft stacked PR [#488](https://github.com/SwordInfra/SwordFS/pull/488),
whose base is the #487 branch `feat/483-slice-record-overlay`, **not** `main`.

## Authoritative design decision: first SliceRecord creates an index

A new ChunkID may be allocated before it is published, but it must **never be
attached in FileMetadata without at least one committed SliceRecord**.
Creation order: allocate ChunkID / SliceID -> persist the data object ->
`CreateIndex(chunk_id, first_record)` -> attach `ChunkIndex -> ChunkID` in
FileMetadata via the #424/#271 publication protocol. An uncommitted orphan
index is acceptable until reclamation (#273); publishing a ChunkID with a
missing index is not. There is **no valid empty private Slice index**.

This refines an earlier hypothetical allowance for an empty attached ChunkID.
No migration, dual representation, empty marker, fake header, or fallback for
that discarded beta-era model is required.

## Data model and operations

`FileMetadata` owns `ChunkIndex -> ChunkID` and EOF/attrs. Only the private
Slice mechanism owns `ChunkID -> ordered SliceRecord history`. Do not move
Slice-specific records into common FileMetadata, and do not change COW.

A single Redis **LIST** per ChunkID contains **only** little-endian 24-byte
`(u32 pos, u64 id, u32 size, u32 off, u32 len)` records, with oldest first.
There is **no LIST header, version preamble, embedded ChunkID, empty sentinel,
or extra Redis index object**. The Redis key namespace already identifies the
ChunkID. Validate every record on read with #483's shared `ValidateRecord`;
any corrupted/non-LIST state is an I/O error, not a sparse hole.

Typed `metadata::slice::SliceChunkMetadata : ChunkMetadata` supports:

- `AllocateChunkID` using the common ChunkID namespace and the existing
  Lua-free `IncrExistingNonNegative` transaction, with `NextChunkID` also
  initialized at `FormatVolume`. The COW mechanism's pre-existing allocator
  behavior remains unaffected.
- `AllocateSliceID` using a dedicated per-volume counter, bootstrapped only
  during `FormatVolume`; the counter must exist and remain a non-negative,
  unexhausted signed 64-bit integer. Preserve `OutcomeUnknown` on ambiguous
  mutation results and never blindly replay/allocate a duplicate ID.
- `CreateIndex(ChunkID, first_record)` writes the **first real SliceRecord**.
  It rejects an already-existing index without appending a duplicate. One-time
  `WATCH`/`LRANGE`/`MULTI`/`RPUSH`/`EXEC` through **existing** `RedisKvTxn` is
  permitted to implement this create-only condition without Lua. No generic
  LIST-length CAS is used for ordinary append.
- `AppendRecord(ChunkID, record)` uses the **native atomic `RPUSHX`** against
  the already-existing LIST; a missing key returns NotFound and must not be
  re-created by a stale subsequent append. This is not a FileMetadata/EOF
  publication transaction; reject invalid caller records before sending.
- `ReadRecords(ChunkID, vector<SliceRecord>*)` uses one `LRANGE`, decodes and
  validates *all* records, and replaces output only on success. An absent or
  missing list returns NotFound. #486 treats an unattached ChunkID as a hole
  via FileMetadata; a **mapped** ChunkID with a missing/invalid list is an
  error, never an empty chunk of zeros.

**No new Lua script** for #484: no script for Slice index initialization,
append, or SliceID allocation. Native Redis commands plus the repository's
existing optimistic transaction adapter are sufficient. Pre-existing Lua in
other established features (COW and lease/locking paths) is out of this PR's
scope and must not be changed under #484.

## LWW and concurrent semantics

Redis list append order is **commit order**. Range resolution is latest
committed covering-record-wins **per byte**, not by SliceID number or client
write-start time; disjoint record updates coexist. The same SliceID can appear
in several records with growing historical `size` after valid tail appends.
Uncommitted/preliminary bytes are not published as visible records.

The record model **permits** historical records sharing a SliceID with
increasing `size`, but the revised #485 writer follows JuiceFS's **freeze on
file Flush**: after a Slice is frozen, later writes use a *new* SliceID and
immutable object. There is no `IDataEngine::Append` requirement. An active
Slice may continue growing in local memory before freeze, but no metadata
record is published for unfinished bytes. #424/#271 still own
**authoritative multi-mount FileMetadata/EOF/zero-cover publication**;
Redis list LWW does not alone prove global commit ordering. Do not introduce
general WATCH/expected-history length CAS for every `AppendRecord`.

An ambiguous response to native non-idempotent `RPUSHX`, or to `EXEC` for
first-record creation or counter allocation, is `OutcomeUnknown`: the mutation
might have happened, and blind retries can duplicate a record or identity.
The full reconciliation protocol remains #271.

## Relevant JuiceFS reference

Upstream `juicedata/juicefs` uses
[`redisMeta.doRead`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/meta/redis.go#L3097-L3103)
with `LRANGE`, and
[`redisMeta.doWrite`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/meta/redis.go#L3140-L3183)
with native `RPUSH`. JuiceFS does **not** store a Slice LIST header or use
Lua to validate one. JuiceFS watches the inode to coordinate the inode
update with RPUSH in one metadata transaction; SwordFS intentionally keeps
its two metadata authorities separate. JuiceFS's local writer
[`findWritableSlice`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/vfs/writer.go#L164-L184)
is the peer reference for **pre-freeze** Slice reuse. The file Flush freezes
active Slices as JuiceFS does; pre-freeze completed Block uploads in JuiceFS
do **not** mean its object storage supports in-place append.

## Verification and exclusions

Tests must cover Redis service-backed first-record creation, concurrent
creation (exactly one winner), no duplicate creation, missing mapped index
failure, native append / LWW order under concurrent disjoint/overlap writes,
remount, same-SliceID size growth, malformed 24-byte records and wrong Redis
types, valid/invalid counters, volume identity separation, and known-vs-
unknown Redis responses with no blind replay. Per-file C++ patch coverage
must exceed 90%; TDD RED/GREEN and full CI are required by the engineering
workflow. No local compilation. Do not add Memory metadata support, new Redis
pools, custom synchronization primitives, full-chunk rewrites, S3 object
updates, or global transaction machinery to this isolated index task.

### Stacked PR merge gate

```text
main
  `-- #487: feat/483-slice-record-overlay
        `-- #488: feat/484-redis-slice-index
              `-- #486: actual Slice reader (base: #484 branch)
```

#486 must supply a real production `ResolveRange()` consumer; only after
integrating **#486 -> #488 branch -> #487 branch** and passing #487's formal
CI (including `dead-code-audit`) may #487 be merged into main. Do not add
fake calls or suppress the audit. #424 gates final #271 publication and #275
activation, but does not block implementing isolated #484 primitives.
