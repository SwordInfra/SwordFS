# #517 — Slice EOF/attachment finalization under concurrent Chunk Flush

Status: mount-local per-inode coordination implemented; review findings under verification.

## Observed incident

The post-#515 full Slice run 40616350589, supported-5, reported an upstream
`generic/346` XUnit PASS (55s), but its original SCRATCH daemon log recorded
`FileReadWriter::Flush chunk FAILED: ino=290 chunk=2 — Slice publication lost
too many EOF/attachment races` at 2026-10-28 01:51:25 UTC. The log belongs to
SCRATCH generation 46 during that test, not to an expected policy denial.
Passing the upstream userspace check alone does not prove confirmed Flush,
release or remount durability. #513 owns the strict per-case writeback gate.

## Authority and race

Every immutable Slice candidate performs an ordered private index append and
an independent conditional inode/attachment metadata finalization. The
precondition contains the inode's EOF and its retained-boundary ChunkID. A
definitely rejected finalization must refresh the snapshot without appending
the same immutable record twice. An unknown reply must fail closed unless
its exact operation can be read back and confirmed.

`FileReadWriter::FlushPendingWritesLocked` currently launches multiple
`Chunk::Flush` operations of the **same inode** in parallel. Each successful
`AttachPrepared`/`FinalizeAttachedWrite` changes that shared inode, invalidating
other in-flight Slice publishers' independent EOF snapshots. Even in the
absence of external mounts or any genuine truncation, the eight application
precondition retries can all lose under heavy concurrent publication.

## Intended correction and non-goals

Preserve parallel physical Slice preparation/immutable S3 PUT across Chunks,
but serialize **only the conditional FileMetadata publication phase** for
Slice Chunks sharing one mount-local FileReadWriter. Retain per-candidate
attachment/EOF CAS, zero-cover/truncate authority, unknown-commit safety and
Window RMW last-writer-wins semantics. Do not change COW's execution policy,
raise retry counts, bypass cross-mount preconditions, or introduce a global
distributed mutex, new Slice encoding, or compatibility mode.

This prevents self-inflicted same-inode conflicts during one local parallel
Flush. Truly competing external mounts or external truncation can still
produce a real, bounded, retryable conflict; they must never be accepted as
success without authoritative metadata confirmation.

The `FileChunkManager` owns a shared, inode-local `utils::FiberMutex` and
passes it through the mechanism-neutral `ChunkFactory::Open` publication-gate
argument. The factory rejects a Slice creation without this gate; the
`SliceChunk` constructor also requires non-null shared coordination rather
than quietly substituting an independent per-Chunk mutex.
Every Slice Chunk performs its RMW preparation and immutable object PUTs
without taking this mutex, then takes the shared mutex only around each
`PublishFrozen` operation (including private record creation/append and the
conditional FileMetadata EOF/attachment transaction). Direct SliceChunk tests
provide their own shared gate; COW does not use the gate. The mutex
is not a distributed lock and does not weaken Redis precondition checks.

## Verification

- RED: **confirmed** on 5a70ccb / Dev Build 40624567592: the test observed
  20 overlapping attachment finalizations with unmodified production, where
  the required peak is 1. The test uses 20 distinct Slice Chunks under one
  inode; the mock suspends conditional attach operations and rejects overlapping
  same-inode metadata finalization as a violated coordination invariant,
  independently of whether a particular race happens to exhaust all 8 retries.
- GREEN: same test under Slice publication coordination, with byte-for-byte
  fresh-reader verification. Suspend the first physical PUT and verify another
  Chunk PUT completes its entry before release, proving metadata coordination
  does not serialize physical uploads. The regression also rewrites existing
  attached ChunkIDs under simultaneous `FinalizeAttachedWrite` attempts,
  checks exclusive finalization, stable attachment identity and exact record
  counts, then verifies a fresh reader sees the complete overwrite. The
  mechanism-level factory test requires fail-closed missing-gate rejection.
  Preserve existing conflict and unknown-reply UT.
- Formal: Debug and Release UT, pinned real COW/Slice `generic/346` with
  **zero** unexpected TEST/SCRATCH daemon writeback, affected supported-5
  shard, full 10/10 strict Slice fstests matrix, and required PR/main gates.
