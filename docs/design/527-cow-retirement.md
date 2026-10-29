# #527 — Retire the concrete COW mechanism

Status: implementation under review in PR #529 (RED and initial GREEN recorded; full CI required).

## Motivation and staged boundary

New volume format defaults to chunk_slice (#518 / PR #520). Primary FUSE, pjdfstest,
fstests and strict asynchronous writeback CI use Slice (#512 / PR #513).
The additional #526 / PR #528 contract tests and full 121-case deletion
inventory are committed to main as a prerequisite of this atomic retirement.
There is no reason to keep the COW concrete implementation as a second data
path. The extensible Chunk mechanism abstraction, however, remains mandatory.

Delete concrete COW implementation, COW private Redis metadata, COW-specific
physical GC and dedicated tests/CI **in one PR**. Do not create an intermediate
commit that runs an incomplete COW registration or silently reads COW metadata
as Slice. #273 Slice GC and #274 compaction and #202 memory are independent;
Slice physical cleanup stays disabled until safety is proven separately.

## Format and mount safety

Persisted ChunkType numeric identities must stay stable:
COW=1 (reserved retired type), Slice=2 (currently supported), RedisCache=3
(future unsupported type). The old COW value remains decodable for useful
diagnostics, but **new COW format and mount of stored COW must fail closed
with a specific unsupported-mechanism error**. Do not reinterpret an old COW
volume as the new Slice default or reject a valid old encoded volume record
merely as malformed. Migration/reprovision must be an explicit offline
decision, not an implicit conversion. Unknown/unsupported values fail closed.

The selection/dispatch and typed metadata capabilities remain mechanism-neutral
so future independent Chunk types can be registered without rewriting VFS.

## GC ownership and deferred cleanup

The removed COW implementation was the sole user of physical ChunkGcWorker.
Keeping its thread/participant implementation would leave a test-only
production API with no valid Slice cleanup policy. Delete this unused worker
and participant instead of disabling static-audit findings. Preserve the real
OrphanReclaimer and common durable reclaim/pending-delete metadata interfaces,
plus the Slice deferred cleanup encoder. #273 must design a safe Slice GC
participant and scheduler on their own merits; no current runtime may delete
Slice objects on the strength of obsolete COW reachability assumptions.

Remove the COW-only Redis `IncrNonNegative` Lua allocator as well: no active
production caller exists; Slice's guarded counter allocation is independently
implemented through `IncrExistingNonNegative`.

## Regression retention

The canonical Stage A inventory is
`docs/design/526-cow-retirement-deletion-inventory.md` (121 historical
COW-fixture tests). The #529 decision ledger is
`docs/design/527-cow-retirement-regression-audit.md`. **Only current,
mechanism-neutral contracts need to survive**: POSIX reads/writes/zero-fill,
EOF/attribute consistency, failure recovery, metadata authority, concurrency,
correct inode lifecycle and deferred cleanup safety. COW-head, revision,
immutable-object key, private CAS/hydration and physical GC behaviors are
implementation details, **not** test-migration requirements for Slice.

All 121 historical cases have a distinct disposition: 74 shared generic
contracts with named witnesses; 13 mixed cases with independent Slice safety
contracts (not COW algorithm compatibility); 34 COW-private-only cases
explicitly retired **without requiring an equivalent test**. This review
criterion applies to the full inventory, not just the latest review comments.
A direct mapping is an executable specification of a real retained invariant;
a `retired-private` annotation is not a requirement to reproduce private
behavior. Retain existing generic Redis identity, namespace/reclaim, VFS,
FileHandle, volume and lifecycle tests on their own merits. Stage A's six
priority generic regressions are independently retained by Stage B.

The same criterion applies beyond the 121 FileReadWriter cases: the five
deleted COWChunk/COWBoundary/WriteBuf/object-key/Redis-head suites contain
another 81 tests (202 historical COW-fixture/unit test declarations in total).
Their private algorithm assertions are retired, while relevant generic
boundaries are already specified independently by Slice, Redis and VFS tests.
See the cross-suite disposition in the Stage B audit; numeric test parity with
the retired COW implementation is explicitly **not** an acceptance condition.

## Verification

- RED first: mounted old COW and new COW format must become unsupported and
  must not create partial metadata. Debug focused GitHub Dev Build.
- GREEN: Debug/Release CI, static-audit/format, mounted Slice E2E, pjdfstest,
  all ten strict Slice fstests shards and zero unexpected daemon writeback.
- Explicit negative persisted-COW tests, unchanged numeric enum identity and
  abstract mechanism-dispatch tests.
- Independent review, merge only with full CI green, and post-merge main CI.

No local native build or local E2E.
