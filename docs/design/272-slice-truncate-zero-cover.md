# #272 — Durable Slice truncate and zero-cover

Status: implementation in progress. Parent: [#201](https://github.com/SwordInfra/SwordFS/issues/201).
The committed beta consistency envelope is [#424](424-slice-cross-mount-ordering.md).

## Problem and invariants

FileMetadata owns inode EOF and logical ChunkIndex attachments; Slice owns ordered immutable object spans inside an attached ChunkID. Cutting the inode EOF alone hides bytes only until a later Grow or sparse Write. A cached `SliceChunk::CaptureBoundary` clamps reads in one mount but is not durable after remount, so previously committed suffixes could return without a Slice-private sanitation record.

- After an ordinarily successful shrink, the discarded portion of a retained boundary must stay zero across Grow, sparse Write and remount.
- Whole logical chunks beyond a shrink detach through existing FileMetadata `CommitShrink`; no zero record and no fresh index is needed for detached chunks.
- The retained boundary is sanitized by appending a zero-cover record for `[visible_prefix, chunk_size)` using the *existing* Slice index's ordered append. The sentinel `SliceID{}` consumes no S3 object. Covering through the chunk end avoids resurrecting older bytes on successive grows.
- Growth from an attached partial old EOF sanitizes that old boundary before making a larger EOF visible; changing attachment or a missing mapped index must not be mistaken for a sparse hole.
- Preserve a mount-local operation barrier across Flush and Truncate, and never retire unconfirmed frozen generations. No new cross-mechanism metadata fields or cross-file log.
- A returned error/OutcomeUnknown does not promise rollback or permission to replay a blind append. A successful regular sequential shrink must have a durable zero cover before it can be regarded as a completed operation.

## Existing execution structure

`FileReadWriter::CommitSizeChangeLocked` plans one mechanism-neutral `RetainedChunkBoundary`, uses `IMetaEngine::{CommitShrink,CommitGrow}`, and asks `ChunkFactory::SanitizeBoundary` to sanitize the retained boundary. `COW` uses a private head CAS; Slice uses its private ordered record stream. `SliceChunk::Read` already interprets zero-cover `SliceID{}` records and overlays local Dirty bytes. The factory must validate the currently mapped ChunkID, avoid silently ignoring a missing attached index, and propagate storage/Redis errors.

Shrinking commits EOF first and appends sanitation second. This maintains immediate EOF clipping; if the later append fails, the operation returns failure and must not claim successful sanitation. Growth sanitizes the old boundary *before* publishing EOF, permitting a subsequent normal Grow to recover sanitation after a failed Shrink append. An unchanged-EOF truncate retry must also sanitize an attached interior EOF before reporting success: if an earlier Shrink committed its EOF but failed private sanitation, successfully updating only mtime would otherwise falsely acknowledge an unsanitized suffix. The same mechanism-neutral factory dispatcher handles this recovery case for both COW and Slice.

This is not atomic across FileMetadata and private Slice metadata: #424 explicitly accepts two
specific exceptional cross-mount failure races. Concurrent cross-mount truncate and EOF-extending
write operations also follow **Slice-record LWW**, not strict operation-level serialization. If an
unchanged-EOF truncate observes an earlier EOF and its zero-cover appends after another mount's
acknowledged write, the later cover can mask the overlapping write even while FileMetadata retains
the larger EOF. The maintainer explicitly accepts this LWW outcome; this does not weaken ordinary
sequential shrink/grow durability or the missing-index fail-closed requirement.

Do not introduce Lua, key renaming, LIST copying, global locking or a transaction coordinator in
#272. A sanitation append costs one Redis record/RTT, no S3 object PUT and no O(history) LIST scan;
#274 later bounds history growth and repeated sanitation amplification.

## Peer: JuiceFS

Compared with JuiceFS `pkg/meta/redis.go` `doTruncate`, which uses ordered `Slice{id=0}` hole/zero records to ensure erased ranges remain zero after EOF grows: **Adopt** the persistent zero-cover semantic and ordered later-write-wins reconstruction. **Adapt** the operation to SwordFS's separate FileMetadata EOF authority and private `ChunkID -> SliceRecords` LIST; use the existing factory boundary dispatch instead of introducing per-inode Slice key paths. **Do not adopt** JuiceFS's inode-keyed chunk index and full transaction pipeline. **Differentiation:** preserve volume-fixed independent Slice and COW metadata, immutable `Put` objects, and explicit #424 beta failure boundary.

## Verification and TDD

- RED/Green: cold Slice remount after retained-boundary sanitation + Grow must read the surviving prefix and zero-filled tail; subsequent Write fills only its new span, repeated shrink/grow remains correct.
- RED/Green: attached missing/invalid Slice index cannot be silently treated as a hole; errors propagate. A detached stale boundary is not republished.
- Redis service-backed: two independent Redis clients/metadata handles validate persisted zero-cover after FileMetadata shrink/grow, cold remount/read, and chunk-boundary detach.
- Failure/ambiguity: no ordinary successful truncate report if sanitation failed; replay/remote races must stay within #424 limitations.
- No COW behavior regression, no full logical-chunk object PUT for sanitation. Truncate-specific changed production C/C++ files require >90% Codecov patch coverage each.

## Deferred integration

#273 owns safe detached-object GC, #274 bounds repeated cover/index growth, #275 owns general public format activation, multi-mount fault orchestration and performance comparison. #202 owns mount-wide dirty buffer backpressure. Do not count those unimplemented capabilities as completed here.
