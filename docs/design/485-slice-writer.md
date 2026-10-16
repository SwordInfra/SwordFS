# Slice Writer — freeze and immutable object publication (#485)

**Status: initial writer implementation under GitHub GREEN verification, 2026-10-16.**
This document **supersedes** the earlier #495 draft requiring native
`IDataEngine::Append`, same-SliceID reuse across durable Flush, and a
provider-specific S3 Express implementation. Those requirements are **removed**.
Canonical Issue: [#485](https://github.com/SwordInfra/SwordFS/issues/485).
Parent #271; global publication design #424; Reader #486; activation #275.

## 1. Correct JuiceFS lifecycle: frozen is not the same as uploaded

JuiceFS's implementation has two *different* concepts:

1. **Logical Slice is writable until `sliceWriter.freezed` becomes true.**
   `chunkWriter.findWritableSlice` finds an eligible local Slice newest-first,
   and permits a write inside the **unuploaded mutable tail**. Once frozen,
   the Slice is never selected for another write.
2. **Some physical Blocks may already have been PUT before the Slice freezes.**
   `wSlice.FlushTo` uploads completed Blocks while the Slice remains active.
   `wSlice.WriteAt` rejects updates at `off < uploaded`. Further writes can
   extend the unuploaded part or create later Blocks, but never modify bytes
   in uploaded Blocks. Final `wSlice.Finish` flushes remaining Blocks after
   freeze. Each physical Block is immutable.
3. **User-visible file `Flush` explicitly freezes all active Slices.**
   `fileWriter.flush` sets `s.freezed = true`, launches `flushData`, and waits
   for the resulting data upload and metadata commit. Subsequent Write after
   that Flush uses a **new SliceID**. It does NOT reuse the same SliceID by
   performing an object-store append. JuiceFS may also freeze automatically
   for size, age, memory, or other local pressure.

Evidence at reviewed upstream source:

- [JuiceFS writer.go, `write`, `findWritableSlice`, `commitThread`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/vfs/writer.go#L130-L232)
- [JuiceFS writer.go, user-visible `fileWriter.flush`](https://github.com/juicedata/juicefs/blob/d4d6874ccc1c7d06b4a0bb32bf74a16f2a232cdb/pkg/vfs/writer.go#L406-L455)
- [JuiceFS cached_store.go, `wSlice.WriteAt`, `FlushTo`, `Finish`](https://github.com/juicedata/juicefs/blob/main/pkg/chunk/cached_store.go)
- [JuiceFS documentation, I/O processing](https://juicefs.com/docs/zh/community/internals/io_processing/)

**Decision:** align SwordFS with these *logical* freeze/Flush semantics.
`IDataEngine::Append` is **not required**. **No** S3 Express-only activation,
`WriteOffsetBytes`, Alibaba OSS `AppendObject`, expected-offset physical
append protocol, append capability probe, or new provider-specific SDK path
in #485. Generic S3-compatible `Put`/`Get`/`Delete` remain sufficient.

## 2. SwordFS first implementation: one immutable object per frozen Slice

JuiceFS maps each logical Slice to **multiple immutable Blocks**, which
allows it to upload complete Blocks before Slice freeze. SwordFS's already
merged #486 Reader instead maps **one SliceID to one object key**:
`SliceObjectKey(id) == "slice/<decimal-SliceID>"`, and executes an exact-range
`IDataEngine::Get` into that object. We must **not silently claim our object
layout is already the same as JuiceFS's**.

For the first coherent implementation of #485:

- **An active, unfrozen Slice remains in sparse local memory buffers** and
  supports in-place local writes or logically contiguous tail growth. A
  `Write` syscall never needs to load a previously published 64 MiB Chunk.
- **Freeze seals the Slice data**; allocate a fresh nonzero SliceID and
  issue **one `IDataEngine::Put(SliceObjectKey(id), payload)`** for each
  sealed, actually written **contiguous range**. On `Put` success, the
  `IDataEngine::Put` contract guarantees the whole immutable object is
  readable; retain the prepared physical SliceID locally.
  #271 will define the production ownership/handoff interface and assemble
  the typed SliceRecord together with authoritative metadata publication.
  Do **not** require a second whole-object GET/HEAD on every successful Put
  merely to re-prove that contract.
- **After freeze, do not write or append to that physical Slice.** A later
  Write to the same or adjacent logical file range selects a new active
  Slice with a fresh SliceID. The reader's per-byte committed-record LWW
  resolves overlap without downloading or rewriting old physical objects.
- **The file Flush boundary freezes all active local Slices** and waits for
  their sealed physical payload preparation. It does not leave their SliceID
  appendable. #271 will finish ordered metadata publication and expose a
  successful durable Flush; until then `Chunk::Flush` cannot report OK merely
  because objects have been PUT.
- When an active Slice becomes too large, **freeze that Slice early** and
  create a new active Slice for subsequent writes. However, an unpublished
  frozen generation must remain locally readable, so **freeze does not
  free or bound its Dirty memory**. Apply admission/backpressure to the
  **combined active + frozen unpublished bytes** and retain data safely on
  pressure; #202 owns the complete mount-wide budget. #275 must not enable
  Slice volumes until that memory gate is satisfied.

The one-object-per-Slice layout avoids modifying #486's Reader and supports
existing MinIO tests. It incurs one PUT for each *frozen* Slice, **not one
PUT per Write syscall**. Cold 4 KiB overwrite still uploads only its modified
Slice (not the full 64 MiB logical Chunk). For long sequential writes, local
contiguous writes accumulate until flush/auto-freeze. This is an intentionally
simpler first layout; it does NOT have JuiceFS's pre-freeze uploaded-Block
streaming optimization. If performance or bounded memory demands incremental
physical uploads while a logical Slice stays active, use immutable
per-Slice Blocks as a **separately designed and tested** next step, including
its unavoidable Reader/object-key changes. Do **not** restore generic native
object Append merely as a substitute for immutable Block layout.

## 3. Slice eligibility and data model

Keep a local collection of active Slice candidates inside `slice::SliceChunk`
separate from the **already committed** Redis Slice history:

- Select the newest **unfrozen, locally writable** Slice compatible with the
  requested logical interval, following JuiceFS `findWritableSlice`'s
  general selection rule. Permit compatible overwrites in the **local
  unuploaded buffer** and contiguous tail extension. Do not overwrite
  previously frozen/published Slices.
- **Sparse coverage is exact, not implied by the physical buffer.** Only
  overlapping or **adjacent** accepted logical intervals may merge into
  one initial immutable SliceRecord, after applying newest local Write
  bytes within that continuous union. If two writes have an **unwritten
  logical gap**, keep separate Slice candidates/records instead of filling
  it with zeros and publishing one wide covering record. Existing committed
  bytes or an implicit hole within that gap must retain their previous
  visibility. Internal zeros are publishable **only** when explicitly
  written, or when separately authorized by #272's EOF/zero-cover semantics.
  A physical layout with padding would require separately bounded records
  for the actually written subranges; **#485 V1 chooses no-gap coalescing**
  to avoid that second encoding and extra Reader complexity.
- A new overlapping or disjoint write that cannot be absorbed into a
  mutable Slice creates another local Slice. Preserve Write acceptance
  order so eventual metadata publication applies overlapping records in
  the same order, regardless of data-upload completion order.
- Freeze terminates local mutability and snapshots the exact data/offset.
  This is the **final Slice length** for that object's initial and only
  authoritative committed record. A historical record's `size` remains
  valid; #483/#484 support multiple records with one ID as a general model,
  but #485 **does not need to publish length-growing records for one ID**.
- For a newly allocated ChunkID, successfully write the first immutable
  Slice object and its **first real SliceRecord before attaching** the
  ChunkID to FileMetadata. A mapped ChunkID with no record is corruption.
  #424/#271 own the global cross-domain commit and ambiguity protocol.

No default WATCH/LLEN CAS, Lua, global Slice coordinator, duplicate metadata
format or compatibility machinery is introduced by this local writer.

## 4. Local dirty generations, read-your-writes, and Flush

Reuse #486's sparse `dirty_` intervals and `utils::FiberRWMutex` rather
than importing COW's whole-prefix `WriteBuf`. At minimum distinguish:

- **Active writes**: accepted local data, immediately visible to local
  `Chunk::Read` over published data or holes.
- **Frozen, uncommitted data generation(s)**: immutable copies or ownership
  transfers prepared by Flush / auto-freeze. They remain readable locally
  until authoritative metadata publication is confirmed; concurrent later
  Write is accepted in a new active generation and wins locally.
- **Physically prepared Slice candidates**: sealed object identity,
  successful `Put` result and pending publication status. The typed
  SliceRecord/commit handoff belongs to #271, not this physical preparer.
  A successful object PUT by itself **does not** make the data visible to
  another mount; do not directly append authoritative
  Redis records before #271 supplies the required commit order.

### Flush boundary and candidate ordering

The existing `FileReadWriter::Write` and `FileReadWriter::Flush` both take a
**shared** `operation_mutex_`, and `Flush` dispatches pending Chunks in
batches. It is **not** currently an exclusive file-wide Write barrier.
Therefore #485 must make the **per-Chunk freeze linearization point**
explicit under its existing fiber mutex:

- A Write accepted **before** that Chunk's freeze belongs to the frozen
  generation; a Write accepted **after** belongs to the next active
  generation. A concurrent Read snapshots either coherent visibility, never
  a half-moved buffer.
- Preserve **mount-local frozen insertion order** for candidates under
  the Chunk mutex, without an unused generation counter. The future #271
  handoff must preserve this order: an older overlapping A cannot be
  published *after* newer B merely because A's `Put` was slower.
  This is local ordering, **not** a Redis record-count CAS,
  distributed timestamp, global transaction or extra writer scheduler.
- This per-Chunk boundary does **not** claim file-wide snapshot isolation:
  #271 must define and test the file-wide Flush acknowledgment barrier,
  cross-Chunk order, `write_epoch_`/EOF interaction and writes racing
  `FileReadWriter::FlushPendingWritesLocked`. Do not change the VFS
  synchronization contract silently to satisfy a local #485 test.
- Before #271 implements authoritative publication, `Chunk::Flush` must
  remain a **truthful failure/NotSupported**, while #485's real production
  preparation pipeline may seal/upload candidates. Do not add test-only
  setters or return `OK` simply because all object uploads completed.
- A repeated or concurrent Flush cannot freeze an **already frozen**
  generation again, emit duplicate publication candidates, or replay its
  `Put` under the same uncertain object key. Track in-flight/prepared state
  through the existing per-Chunk synchronization and report an honest
  status. A subsequent newly accepted Write may form a *new* generation;
  it must never be mistaken for a retry of the previous one.

### Dirty Read is an explicit #485 implementation deliverable

**Do not defer the local Reader changes to #271 or consider them completed
by the existing #486 code alone.** #486 currently snapshots *one* sparse
`dirty_` map under `utils::FiberRWMutex`, fetches only uncovered published
object subranges, then overlays those accepted bytes. #485 must adapt that
existing *production* `SliceChunk::Read` / `Write` path when it introduces
active and frozen generations. Keep the #486 `ReadRecords -> ResolveRange ->
IDataEngine::Get` published-data path and Slice object key unchanged.

The required invariants are:

1. **One logical dirty-visible view.** Reads see the per-byte latest
   **accepted local Write** across *all* unpublished active and frozen
   generations. An older frozen generation supplies bytes only where no
   later accepted Write superseded it. A reader need not understand physical
   upload state: it consumes a merged, sparse visible interval projection.
   Reuse #486's `dirty_` map as that projection where feasible; **do not
   duplicate or drift** a second mutable truth for visibility. Upload-owned
   frozen buffers are separate immutable lifetime/provenance state, not
   another competing Reader cache.
2. **Atomic freeze boundary.** Under the existing fiber mutex, freezing
   transfers/snapshots the accepted pre-freeze data into an immutable
   generation, while subsequent Writes enter a **new** mutable generation.
   A concurrent Read takes a coherent snapshot of the visible dirty
   intervals at one point in this ordering. Never hold the mutex across
   Redis/S3 I/O, and never expose a temporary hole when the generation is
   moved from active to frozen.
3. **Overlay precedence.** Use the visible dirty snapshot to mask published
   bytes, uncovered zeros and zero-cover records *only in the intersected
   byte ranges*. Different-generation overlaps resolve by accepted Write
   order, **not** by SliceID, freezing order, Put completion, or metadata
   commit order. Do not rehydrate a whole published Chunk, nor issue physical
   GETs for ranges completely covered by local dirty bytes. Keep the
   read-result commit-at-end guarantee on metadata or object I/O failures.
4. **Retirement is gated by authoritative commit.** A successful immutable
   `IDataEngine::Put`, successful preparation, failed Flush, or ambiguous
   Redis publication **must not clear** frozen bytes from the local visible
   projection. **#485 leaves such bytes retained** because #271 has not
   yet implemented commit acknowledgment. Later #271 may retire a frozen
   generation only after confirming its own authoritative publication and
   switching the Reader's published baseline; retirement must never delete
   newer active Write bytes, including when both generations cover the same
   logical offset. Use provenance or rebuild the projection from remaining
   uncommitted generations; a blind `dirty_.erase(range)` on Flush success
   is invalid.
5. **Failure and lifecycle safety.** On Put failure, timeout, lost response,
   retry with fresh SliceID, cancellation, or later overlapping Write, both
   older uncommitted bytes and newer accepted bytes remain available for
   reads and retry until explicitly superseded/committed. The `Chunk`
   session must not be evicted as clean while either active or frozen
   unpublished data remains (`HasPendingWrites` must reflect both). Local
   `TruncateLocal` and boundary sanitation must remove only logically
   truncated dirty bytes across all generations, without reviving frozen
   suffixes or corrupting the #486 published-boundary provenance rules.

**Concrete end-to-end example (same Chunk session):** published `abcdefgh`
in `[0,8)`; accept Write A = `WXYZ` at `[2,6)`; freeze A and block its Put;
accept Write B = `12` at `[4,6)`; `Read(0,8)` must return `abWX12gh` while
the Put is in-flight, after the old Put succeeds but remains uncommitted,
and after an old Put failure. On a *known* later authoritative publication
of A, the local view must still show B until B is published. No GET is
needed for the bytes `[2,6)` while A/B are locally visible.

**#485 must implement and test the Active/Frozen-aware Dirty Read integration
in the same PR as the Writer**, rather than leaving a separate unfunded
Reader fix. True publication-confirmed Dirty Range retirement is the
explicit #271 integration gate, with its required regression test.

On any data preparation error, **preserve all accepted local writes** and
report an error. An ambiguous `Put` outcome cannot be assumed to have left
no object; never overwrite an identity that could already be referenced.
For safe retry, use a **fresh SliceID/object key**, retaining the frozen
data buffer; unreferenced orphans may remain for #273. Because each frozen
object is immutable and uniquely named, this is much simpler than ambiguous
in-place Append reconciliation.

**Put success/ambiguity proof boundary:** the existing
`IDataEngine::Put` contract says `OK` means the entire object is atomically
readable. #485 may rely on that contract (and test it on MinIO), without a
redundant unconditional whole-object verification GET. For a non-OK or
ambiguous response, do not treat the object as published, do not delete the
possibly live key or blindly reuse it; retain local bytes and prepare a
fresh identity when retry is safe. Distinguish **physical Put completion**
from **authoritative SliceRecord/EOF commit** at every failure path.

`Chunk::Flush` must continue to return a truthful error/NotSupported
until #271 implements the typed candidate consumer, creates authoritative
SliceRecords and publishes FileMetadata/EOF successfully. #485 preserves
physical object identities and frozen insertion order internally; it does
**not** ship an unused public export API or claim to deliver typed intents.
No fake successful Flush, no data loss on interrupted preparation.

## 5. Ordering, correctness, and lifecycle scenarios

| Scenario | Required behavior | Owner |
|---|---|---|
| 4 KiB cold overwrite inside 64 MiB published Chunk | Write local 4 KiB; PUT changed Slice only on freeze; no 64 MiB GET/PUT | #485 |
| Multiple adjacent writes before Flush | Reuse mutable local Slice, coalesce into one immutable PUT when frozen | #485 |
| Disjoint writes with an unwritten gap over existing data | Two bounded SliceRecords (and V1 physical Slice objects); **never** zero-fill the gap into one covering record | #485 |
| Write → Flush → Write → Flush | Two **different SliceIDs**; both objects immutable, no object Append; later committed record overlays earlier if needed | #485, #271 for success |
| Full Block could be uploaded before freeze as in JuiceFS | Not in initial single-object design; needs separate immutable-Block layout/Reader change | future measured enhancement |
| Concurrent Write during in-flight Flush | New active generation remains visible; frozen generation cannot absorb new bytes | #485 |
| Two overlapping frozen candidates | Frozen insertion order preserves accepted generation precedence; #271 must construct and publish typed records in the same order regardless of physical completion | #485 local / #271 handoff and commit |
| Auto-freeze under pressure while publication remains pending | Keep all active + frozen Dirty bytes accounted; freeze alone cannot relieve memory pressure | #485 local / #202 budget |
| Read while A is frozen/in flight and newer B overlaps A | Read per-byte newest accepted A/B plus untouched published ranges; no visibility gap or redundant GET for dirty-covered bytes | **#485 Dirty Read** |
| Immutable Put succeeds, metadata still uncommitted | Frozen A remains locally readable, `HasPendingWrites` remains true; no early dirty eviction | **#485 Dirty Read** |
| Publication of old A succeeds after newer local Write B | B still wins locally; retirement of A must not erase B | #271 commit retirement, tested with #485 projection |
| Later overlapping different Slice / zero-cover | Preserve per-byte LWW committed order; no previous immutable object rewrite | #485 locally; #424/#271 globally |
| Data Put succeeds but metadata commit fails | Object remains uncommitted; preserve accepted local data, no false durable Flush | #271 |
| S3 Put response ambiguous | Keep bytes, use fresh identity for safe retry, allow orphan old key | #485 / #273 |
| Shrink/grow from different mounts | No stale zero-cover, attachment or write may erase acknowledged later bytes | #424/#271/#272 |

Object-storage requests remain on the existing `BlockingExecutor`; no new
thread pool or SDK protocol. `IDataEngine` stays **unchanged** in #485.

## 6. Test-first plan and explicit non-goals

Before C++ implementation, establish focused GitHub Dev Build **RED** on
the actual `SliceChunk::Write`/Flush-generation behavior; then GREEN and
formal CI. Unit/service-backed tests must cover:

- Adjacent local writes coalesce into one frozen Slice; random writes
  produce sparse immutable candidates without whole-Chunk hydration.
- **Gap correctness (required):** published `abcdefgh`, Write `X` at byte
  1 and `Y` at byte 5, then freeze candidates; the visible bytes must be
  exactly `aXcdeYgh`, **not** `aX\0\0\0Ygh`. Assert that no SliceRecord
  claims logical coverage of unwritten `[2,5)`; separately verify adjacent
  or overlapping writes coalesce without losing last-Write precedence.
- Confirm a successful `IDataEngine::Put` provides the atomic readable
  object expected by its existing contract, without an automatic second
  whole-object GET per write. Test ambiguous/non-OK Put does not publish
  or delete an uncertain key and retains Dirty Read.
- `Write` while an older generation is frozen/preparing, including local
  read-your-writes, overlapping generations and failing Put; no data loss,
  output corruption or premature remote visibility.
- **Dirty Read regression matrix (required in #485, not optional #271):**
  published data + accepted A + frozen A + overlapping newer B; check exact
  `abWX12gh` bytes across the freeze boundary, after a successful-but-
  unpublished Put, and after failed/ambiguous Put. Verify no dirty-visible
  gap during freeze, latest-accepted-write precedence despite reverse Put
  completion, and zero GET bytes for fully dirty subranges. Repeat with
  A/B disjoint, sparse holes, zero-cover, and a local truncate removing
  frozen tail bytes. Ensure `HasPendingWrites` stays true when active is
  empty but an unpublished frozen generation remains.
- Snapshot consistency and failure atomicity: Read concurrent with Write/
  freeze returns a coherent snapshot of one accepted-write order, not a
  partially moved buffer; late published-object GET failure must leave
  the caller's output unmodified. Do not change #486's published-data range
  resolution or physical key format as part of this adaptation.
- A frozen Slice cannot accept further data. **Write after Flush prepares a
  new SliceID**. Data GET on already committed old Slice remains unchanged.
- Multiple frozen candidates are retained in freeze insertion order under
  the Chunk lock. #485 tests distinct immutable objects, newer local Write
  precedence and absence of early metadata publication; #271 must add its
  production typed-record consumer, snapshot/order verification and
  concurrent completion/commit tests when the real publication protocol exists.
- Freeze races with Write under the **actual shared VFS operation lock**;
  demonstrate correct per-Chunk ordering, then record cross-Chunk and
  file-wide Flush barrier tests under #271. Track total **active + frozen**
  pending Dirty bytes, including failed uploads; auto-freeze must not make
  an uncommitted buffer eligible for clean eviction or falsely release its
  memory budget.
- Repeat/concurrent Flush after an earlier failed/NotSupported preparation:
  no duplicate freeze, no duplicate record candidate, no unsafe repeat PUT
  on an uncertain SliceID, and newer Writes stay distinguishable from the
  previous frozen generation.
- A 4 KiB overwrite does not cause 64 MiB fetch/put; object count is per
  frozen coalesced Slice rather than syscall count.
- Failed/ambiguous Put preserves locally accepted bytes; fresh SliceID
  retry does not rewrite potentially live old objects.
- No Slice-specific runtime or COW changes expose fake successful
  persistence before #271; existing Reader/hole/zero-cover behavior passes.

**Deferred to #424/#271**: linearizable multi-mount publication, fully
durable successful Flush, first ChunkID attachment commit, stale-session
revalidation, full crash-recovery matrix and Redis ambiguous record publish.
**Deferred to #272/#273/#274**: truncate/zero-cover, reclaim and compaction.
**Deferred to #275**: enabling Slice volume after all gates pass.
**Deferred optimization**: JuiceFS-style multiple immutable Blocks per live
Slice, requiring a new physical mapping and changes to #486.

All changed C/C++ production files require **>90% Codecov patch coverage**,
TDD RED/GREEN through GitHub CI and an independent implementation review.
No local SwordFS compilation. No backward-compatibility layer for prior
beta design, because #485's native Append requirement was never implemented.

## 7. Revised delivery sequence

1. **Finalize this reviewed design**: ordinary S3 `Put`, immutable physical
   Slice objects, JuiceFS-style freeze-on-Flush and no new `IDataEngine`
   method. Update stale Issue claims and the Draft PR before implementation.
2. Implement the **local** active/frozen Slice selection **and integrate
   Active/Frozen-aware Dirty Read in `SliceChunk::Read` in this very Issue**.
   Prove cold small-write, no visibility gap at freeze, write-during-Flush,
   newest local Write wins, and no early dirty eviction with RED/GREEN tests.
3. Integrate prepared immutable `Put` objects and unpublished Slice
   candidates, verify retries and fault injection, patch coverage and full CI.
4. **#271** owns constructing the typed ordered SliceRecord handoff from
   physical candidates plus authoritative publication and file-wide Flush
   acknowledgment; only then
   implement successful persistent `Chunk::Flush`, validate inter-mount
   concurrency and enable Slice later under #275.

## 8. Initial #495 implementation checkpoint

The implementation now under verification keeps two maps inside
`SliceChunk`: `active_` is the unfrozen, contiguous-written-range-only
collection; `dirty_` remains the **single Read-visible projection** of all
locally accepted writes, including frozen, unpublished generations.
`AcceptWrite` coalesces only touching intervals and overlays newer bytes.
It fast-paths contiguous tail append and in-range overwrite to avoid
quadratic repeated prefix copying. A 4 MiB mutable-Slice watermark
auto-freezes Active intervals, including splits of a larger single Write;
this bounds mutable candidate size **without** releasing unpublished bytes.
When a new overlapping Write would grow Active coverage past the watermark,
the writer freezes the prior Active candidate **before** accepting the new
range; repeat overwrites that add no newly covered bytes do not trigger a
freeze. This also bounds the union of two large overlapping Writes without
covering a sparse gap with implicit zeros.
`Read` snapshots that projection before metadata/S3 operations. No global
visibility is claimed by a successful physical upload.

`Flush` atomically moves the current Active intervals into ordered frozen
candidates under the Chunk fiber lock. Each candidate's immutable payload
is uploaded with a fresh `SliceID` and the existing `IDataEngine::Put`.
Frozen candidates use stable shared ownership: a concurrent Write can trigger
more auto-freezes without invalidating a candidate whose Put is in flight.
Successful Put retains the physical `SliceID` without claiming an exposed
metadata intent; known or
ambiguous failure retains its bytes and retries under a **new** physical
identity. Prepared candidates are never re-PUT on subsequent Flush calls.
Preparation is serialized **per Chunk**, preserving frozen insertion order
without needing an out-of-order upload-completion scheduler. The typed
consumer and any stable snapshot/ownership transition are explicitly #271.

Local `TruncateLocal` clips the merged visible ranges and any partially
visible frozen candidate; completely invisible candidates must be retired
and their buffers freed as soon as any in-flight Put has released ownership.
A later #271 consumer must never construct an invalid zero-length record.
Until #271 supplies a globally correct commit/acknowledgment protocol,
`Flush` continues to return `NotSupported` after successful preparation.
The candidate payloads and visible dirty bytes cannot be released on Put
alone. No change is made to COW or `IDataEngine`.

This checkpoint is **not** a successful durable Slice writer. Expanded GREEN,
complete coverage, mount-wide #202 backpressure, integration review,
formal CI, and authoritative publication are distinct unfinished gates.
The final authoritative commit belongs to #271, while mount-wide admission
remains #202 and gates activation under #275.

## 9. Review F1/F2 responsibility decision (2026-10-17)

**F1 is in #485.** A logical nonzero-EOF shrink can make an entire
already-frozen, never-committed Slice invisible. On that transition,
remove its candidate from Chunk ownership and release its payload; a
`Flush()` snapshot that is currently inside `IDataEngine::Put` keeps the
candidate alive only until the I/O returns. An orphan object is harmless,
but an invisible candidate must not remain eligible for typed record
publication, and must not retain the 4 MiB payload indefinitely. A partial
shrink keeps the immutable payload while recording only the surviving
visible prefix. The local `dirty_` projection continues to enforce LWW.

**F2 is explicitly reassigned to #271 as its mandatory, testable prerequisite**
to successful Slice publication. A typed handoff cannot yet have a genuine
production consumer because #271's authoritative cross-domain transaction,
freeze acknowledgment, attachment fencing and Dirty retirement have not
been implemented. Adding a `SliceChunk` getter accessed only by tests now
would violate SwordFS's no-test-only-production-symbol gate; retaining an
unused `generation` and private `SliceRecord` falsely suggests the seam is
already delivered. Instead, #485 guarantees per-Chunk frozen insertion order,
physical `SliceID` from a successful immutable `Put`, payload extent and
remaining visibility. #271 must define the real production consumer interface,
assemble validated `(pos,id,size,off,len)` records from these candidates,
test multiple disjoint/overlapping generations and truncate/late-Put cases,
and publish them with authoritative FileMetadata/EOF in correct commit
order. Until that integration exists, `Flush()` remains `NotSupported`.

This is a scope correction, **not** a claim that #485 implements record
handoff. The #271 parent Issue must retain this prerequisite explicitly,
including tests and reviewer acceptance, before permitting #275 activation.
