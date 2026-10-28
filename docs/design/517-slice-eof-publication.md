# #517 — Slice EOF/attachment finalization under concurrent Chunk Flush

Status: #517 merged in #519; post-merge factory encapsulation review tracked by #523.

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

The mount-owned `ChunkFactory` encapsulates a Slice-only publication registry
that resolves a `utils::FiberMutex` by inode. Every Slice Chunk constructed
from that factory receives the same shared gate for its inode, even when
separate callers open it independently. The factory's **public** `Open` API
has no mechanism-specific parameter: VFS `FileChunkManager` neither owns
the gate nor knows about Slice internals. COW uses no publication gate.

The private registry stores weak references so a gate remains alive as long
as any Slice Chunk uses it, and reclaims stale inode entries periodically.
Distinct inodes get distinct gates; the registry mutex protects only the
short gate lookup/creation, not IO. The `SliceChunk` constructor still
requires non-null shared coordination and refuses an independent fallback.
Every Slice Chunk performs its RMW preparation and immutable object PUTs
without taking this mutex, then takes the shared mutex only around each
`PublishFrozen` operation (including private record creation/append and the
conditional FileMetadata EOF/attachment transaction). Direct SliceChunk tests
share one gate through a dedicated test constructor helper. The mutex
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
  mechanism-neutral factory test checks that callers no longer need to inject
  a Slice-specific gate. Preserve existing conflict and unknown-reply UT.
- Formal: Debug and Release UT, pinned real COW/Slice `generic/346` with
  **zero** unexpected TEST/SCRATCH daemon writeback, affected supported-5
  shard, full 10/10 strict Slice fstests matrix, and required PR/main gates.

## Verified acceptance evidence

- Fix PR #519: original HEAD `cc65107`, merged main `491eb5f`. Full PR CI
  [40631426458](https://github.com/SwordInfra/SwordFS/actions/runs/40631426458)
  succeeded. Real COW `generic/346` PASS 52s, Slice PASS 82s, and original
  TEST/SCRATCH daemon logs had zero unexpected Flush/writeback errors.
- Combined Slice-primary (#513) plus #519 fix, exact integration HEAD
  `c781d30`: [strict CI 40641558590](https://github.com/SwordInfra/SwordFS/actions/runs/40641558590)
  succeeded. All 10 Slice shards PASS; 645 selected, 644 executed, one
  explicitly deferred, 145/145 supported PASS, 499 classified known gaps,
  zero aggregate blockers. The aggregator checked `chunk_type=chunk_slice`.
- Post-merge main CI
  [40642230374](https://github.com/SwordInfra/SwordFS/actions/runs/40642230374)
  succeeded. The #523 structural review follow-up must independently pass
  its current HEAD's GitHub CI; historic evidence does not replace that gate.
