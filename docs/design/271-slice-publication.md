# #271 — Publish Frozen Slice candidates through the existing metadata model

Status: **IMPLEMENTED IN DRAFT PR #504 / focused GREEN; final patch coverage and required CI pending**
(2026-10-20).
Main issue: [#271](https://github.com/SwordInfra/SwordFS/issues/271).
Parent: #201; finalized beta contract: [#424](424-slice-cross-mount-ordering.md).
Dependencies: #483/#484 model/private index; #485 physical freeze/Put;
#486 reader. Separate #272 sequential Truncate/Grow, #273 GC, #274
compaction, #275 activation.

## Original missing production path (before #271)

`SliceChunk::Write` accepts byte-range dirty updates into `active_` and
`dirty_`; `FreezeActiveLocked` captures immutable `FrozenSlice`
candidates in freeze order; `SliceChunk::Flush` allocates unique SliceIDs,
PUTs immutable objects, then originally returned `NotSupported` without
constructing or publishing `SliceRecord`. Before this change there was
no production consumer of prepared candidates, and file-wide
`FileReadWriter::Flush` could not report success for Slice.
The #271 implementation replaces that publication-disabled path;
`SliceChunk::Read` continues overlaying local dirty ranges on
published SliceRecord LWW reads.

Current useful infrastructure:

- `SliceChunkMetadata::CreateIndex` creates an index with its first
  real record, `AppendRecord` is native Redis `RPUSHX` for existing ID,
  `ReadRecords` reads the committed ordered index.
- `IMetaEngine::ReadFileChunkSnapshot`, `AttachPrepared` and
  `FinalizeAttachedWrite` perform typed common FileMetadata operations;
  `FileSizePrecondition{eof,eof_boundary}` provides conditional
  attachment/size publication without adding Slice-specific fields to
  common metadata.
- `FileReadWriter::FlushPendingWritesLocked` captures the file-wide
  write epoch, waits for every submitted chunk Flush to return, and
  retires its live EOF view only on success.

## Contract and non-goals

- Ordinary Freeze/Put is **not** a published write. A successful Flush
  must mean each candidate covered by its barrier was published to
  the correct attached ChunkID, and size/mtime/ctime were updated.
- Preserve the actual logical byte intervals; never pad a sparse gap
  with an implicit-zero covering SliceRecord. No 64-MiB hydration.
- Preserve local Write acceptance order for overlapping frozen
  candidates regardless of physical PUT completion order. A newer
  local Write during an older Flush must remain visible and pending.
- On failed definite or unknown publication, never discard locally
  accepted bytes or claim success. A remote mutation with lost reply
  is *ambiguous*, not rolled back; reconcile an immutable, unique
  SliceID record or return OutcomeUnknown without blind duplicate
  append. Unreachable objects/indexes may leak.
- For initial attachment create a nonempty private list before
  FileMetadata binds the new ChunkID. On losing a first-attach race,
  reload the attached identity and rebase or fail; a fresh private
  losing list can leak.
- **Accepted #424 beta exceptions:** a failed EOF-extending write
  may later expose unacknowledged tail, and failed stale zero-cover
  can mask confirmed bytes. Do not add global Lua/transaction
  machinery to fix them. These exceptions do **not** allow ordinary
  false Flush success, losing in-EOF records, or unsafe GC.
- **Owner-accepted in-EOF partial-visibility semantics (2026-10-21):**
  a valid private `RPUSHX` on an already attached ChunkID may become
  readable on another mount even if the subsequent independent inode
  EOF/mtime/ctime finalization fails and the local Flush returns
  an error. A failed Flush means its entire sequence was **not
  confirmed**, not that previously successful Redis commands were
  undone or rendered invisible. Keep the affected local Frozen/Dirty
  candidate for safe retry, never falsely report success, and avoid
  duplicating its SliceRecord. This is an explicit beta consistency
  decision, **not** a promise of failed-Flush atomic rollback; do not
  introduce a transaction system merely to hide the already appended
  record. A fresh-reader regression test records this behavior.
- #272 owns the persistent ordinary shrink/grow sanitation primitive;
  do not add a new `valid_length` or zero-cover framework in #271.

## Candidate publisher state and lifetime

Keep candidate representation **inside SliceChunk**; no production
test-only exporter or generic FileMetadata interface. Augment each
`FrozenSlice` with publication progress, as necessary:

1. `prepared_id` denotes a successfully uploaded immutable object.
2. The target `ChunkID`/appended-state is retained after a confirmed
   native `RPUSHX` or first-record index creation, to prevent an
   ordinary retry from appending that record again.
3. A failed or ambiguous index mutation is resolved by looking for
   the unique `SliceID` and exact intended record on the selected
   attached index, or returned to the caller as uncertain. An absent
   record following an uncertain outcome does not authorize blind
   replay.
4. A successful FileMetadata attach/finalize permits **retiring only
   that exact frozen candidate** from `frozen_`.

The immutable physical payload and its possibly shortened
`visible_len` remain owned by `shared_ptr<FrozenSlice>` across
Fiber/S3/Redis suspension. `TruncateLocal` can shorten or remove an
unpublished candidate while PUT is in flight; before issuing any
metadata command take a coherent visible-length snapshot. If it
changes during publication, do not silently retire it; return a
conflict for #272's eventual precise truncate integration.

### Rebuilding Dirty Read after confirmed partial retirement

Current `dirty_` is a merged LWW projection containing all Frozen
and Active candidates; erasing a byte range on candidate completion
can accidentally discard a newer Write to the same range. Instead,
under the existing FiberRWMutex, rebuild `dirty_` in acceptance
order from the **remaining** `frozen_` candidates followed by
`active_`. Reuse the existing `AcceptWrite` merging implementation
rather than adding new provenance API to the generic Chunk class.
**Batch retire confirmed candidates and rebuild once per Flush, including
the committed prefix before reporting a later candidate failure.**
Build a set of confirmed candidate identities and filter the pending
Frozen vector in **one pass**: repeatedly erasing one element from the
front of the vector would still incur quadratic movement for thousands
of sparse frozen records. Avoid rebuilding all dirty payloads after
every Slice, which would likewise be quadratic. #202 owns the total
memory backpressure.

## Minimal commit algorithm — reuse typed interfaces

For each candidate in freeze order:

1. Ensure immutable PUT succeeded; allocate a fresh SliceID on an
   uncertain PUT retry, never overwrite a maybe-live object key.
2. Build validated `SliceRecord{pos,id,size,off=0,len=visible_len}`
   from the published frozen payload. This is a real internal typed
   consumer; bytes outside the candidate's visible prefix cannot
   be published.
3. Read current `FileChunkSnapshot`. If an attachment exists,
   append once with `AppendRecord` (native `RPUSHX`); then call
   `FinalizeAttachedWrite` for actual EOF/mtime/ctime. If no
   attachment, allocate fresh ChunkID and `CreateIndex` first;
   publish with `AttachPrepared`.
4. On a known FileMetadata precondition conflict re-read and retry
   against the current attachment. Rebase to a new ChunkID if the
   mapping changed. A record previously appended to a detached
   ChunkID is unreachable garbage. Keep work bounded; surface
   persistent contention as a retryable error and retain Dirty Read.
5. If an append succeeded but FileMetadata finalization failed,
   preserve that append-state for retry. Use the unique immutable
   candidate's record identity for unknown reply reconciliation.
   No blanket transaction or LIST-length CAS for ordinary appends.
   In particular, **a newer inode EOF reaching the proposed end is not
   proof that this candidate's inode side effects were committed**:
   another writer may have extended the file after our own distinct
   SliceRecord append. FileMetadata must reject a stale EOF
   precondition even when the observed EOF is already large enough.
   The publisher then refreshes its snapshot and finalizes the same
   previously appended candidate, without repeating RPUSHX. The
   refreshed mutation rechecks the current inode write policy and
   updates mtime/ctime. A previously lost response does not confer
   permission to bypass an immutable inode that was changed later.
6. Only after confirmed metadata success mark the candidate retired.
   Batch-remove confirmed candidates and rebuild the Dirty projection
   once after the Flush attempt, including after a later candidate
   fails. Multi-chunk Flush may partially publish before a later
   error, but must not falsely acknowledge the entire Flush.

**Important race to prove:** concurrent Write may happen during
Frozen PUT or Redis commands, while FileReadWriter's operation
barrier uses a shared rather than exclusive lock. It must stay in
the newer Active generation. No API may retire data solely by
position because an older published candidate can overlap it.

## JuiceFS peer check

JuiceFS `pkg/vfs/writer.go` freezes active Slices on file Flush,
uploads immutable physical blocks, and `chunkWriter.commitThread`
commits slices in logical creation order before reporting completion.
Its Redis `doWrite` records Slice/EOF changes in a small watched
metadata transaction. SwordFS retains native RPUSHX and separate
FileMetadata authority; the two precisely documented #424 race
exceptions are accepted only for beta. Do not copy JuiceFS's
block layout, full transaction framework, or append-store protocol.

## TDD acceptance matrix (real production consumer)

1. First write to an empty Chunk produces a readable nonempty
   attached index, correct EOF and no whole-chunk Put.
2. Two disjoint / overlapping writes, including separate Flushes,
   preserve prior uncovered bytes, later record wins in overlap,
   remount/second reader reproduces committed bytes.
3. Several frozen candidates before Flush keep their acceptance
   order even if an earlier Put was delayed and later Write was
   accepted during it. Only confirmed generations retire.
4. First attachment loses a race: new/current mapping stays
   authoritative; retry/rebase or clean failure; no lost Dirty Read.
5. Definite Put/append/finalize failure and ambiguous Redis append:
   no false Flush success, no dropped locally accepted bytes,
   no duplicate record on ordinary retry.
6. A nonzero index across Chunk boundaries computes the right
   absolute EOF; file-wide Flush waits for each participant.
7. No change to existing COW behavior; #272 normal truncate/grow
   validation and #273/#274/activation remain separately gated.

Tests are written **first**, pushed to focused GitHub Dev Build
`swordfs_test` with a Slice GTest filter and verified RED before
production implementation. Do not compile C++ on the development
host. After GREEN run formal PR matrix, Codecov patch >90% for each
changed production file, independent review and merge only when
all gates genuinely pass.

## TDD / implementation progress — 2026-10-20

- **RED:** [Dev Build 39403430366](https://github.com/SwordInfra/SwordFS/actions/runs/39403430366)
  compiled the original three consumer-facing tests and failed all
  three on the expected production `NotSupported` publication path.
- **GREEN for first three cases:** [Dev Build 39404418121](https://github.com/SwordInfra/SwordFS/actions/runs/39404418121)
  passed after the first production implementation.
- The initial Slice-only publisher constructs typed records from
  Frozen candidates, creates the first private index before
  FileMetadata attachment, appends natively on existing attached
  ChunkIDs and finalizes inode EOF/attrs using existing typed APIs.
  Confirmed candidates are retired and the Dirty Read projection
  rebuilt from later Frozen and Active generations.
- Metadata append progress is remembered across failed size-finalize
  retries. The publisher probes for the exact unique record after an
  ambiguous Redis reply, never blindly retries an unresolved RPUSHX.
  A first-attachment loser may rebase to the attached winner.
- Existing physical-preparation tests originally asserted `ENOSYS`
  for every Flush; the final suite now distinguishes normal successful
  publication from explicitly injected FileMetadata failures.
  Required final CI, patch coverage and independent review remain
  **pending**. This is **not** a statement that #271 is finished or
  ready to merge.
- **Extended GREEN:** [Dev Build 39406739224](https://github.com/SwordInfra/SwordFS/actions/runs/39406739224)
  passed the full `SliceChunkTest.*` and real
  `RedisSliceChunkMetadataTest.*` suites after adding first-attachment
  competition and definite/ambiguous publication failure cases.
  A final additional partial-file-Flush test explicitly verifies
  confirmed-prefix retirement and failed-suffix retry without
  duplicate RPUSHX or duplicate immutable Put.
- **Further GREEN:** [Dev Build 39407373922](https://github.com/SwordInfra/SwordFS/actions/runs/39407373922)
  passed extended SliceChunk and RedisSliceChunkMetadata suites after
  the partial-Flush test was added. The newest
  [Dev Build 39408018732](https://github.com/SwordInfra/SwordFS/actions/runs/39408018732)
  tests ambiguous first-index and first-attachment responses as well.
- Added a new **real Redis two-client** °SliceChunk::Flush° integration
  test under °RedisSliceChunkMetadataTest°: two independent
  °RedisMetaImpl° backends share actual Redis FileMetadata/ChunkMetadata,
  publish an initial sparse Slice, then perform a second client's
  overlapping overwrite with a fresh reader on the first client.
  The object store is a test in-memory IDataEngine to isolate the
  metadata path; this is **not** #275's eventual FUSE/S3 E2E test.
  GitHub Dev Build 39409839658 confirmed the completed real Redis
  fixture/test suite **GREEN**; this is still not #275 FUSE/S3 E2E.

## Final focused review and verification additions

- GitHub Dev Build 39415350266 **GREEN** on four additional semantic
  cases: bounded repeated EOF contention, one transient EOF conflict,
  first attachment whose outcome is unknown without a committed
  mapping, and correct file EOF at a nonzero ChunkIndex. None
  adds a new production hook; fault injection belongs to test doubles.
- A further focused test group exercises two real file-offset bounds:
  an out-of-range ChunkIndex and a valid final ChunkIndex whose
  exclusive byte end would exceed `kMaxSupportedFileSize`. Neither
  may create an attached index or acknowledge Flush.
- An unknown first-attach result with only a mapped ChunkID and no
  committed EOF must preserve Dirty data until size finalization
  succeeds. An ambiguous append with a persisted record that differs
  from the intended SliceRecord must not count as success or be
  blindly replayed.
- These are **error and publication boundary tests**, not a new
  requirement to make the two #424 beta-exception races atomic.
  Per-file Codecov patch coverage **strictly above 90%**, final
  PR CI and requirement-first review remain gates to Ready.
- Issue #505's FUSE-hosted fstests classifier reason mismatch is
  independently fixed and merged through PR #506; keep its exact
  two-test NOTRUN normalization separate from #271's production code.

## P2 review correction: EOF precondition is not a commit token

The independent review found that the existing
`RedisMetaTxn::FinalizeAttachedWrite` treated a stale growth request as
already completed whenever `expected.eof < end && inode.size >= end`.
This rule was intended to accept lost-reply replay of an already
successful EOF extension. It is unsafe for Slice publication because
the SliceRecord being finalized might be a *new* append, unrelated to
the operation that advanced EOF. EOF equality is not an operation
identity, and a false success can omit a fresh mtime/ctime touch or
bypass a newly immutable inode.

The corrected rule is a conventional optimistic FileMetadata
precondition: a changed EOF returns a conflict, irrespective of
whether the new EOF exceeds the proposed write end. A retry refreshes
the authoritative snapshot, checks the current write policy, and
finalizes the still-pending candidate. The candidate's own successful
SliceRecord append must not be repeated. Legitimate ambiguous
replays may need to perform an additional timestamp touch or return
a current-policy error rather than claiming certainty from EOF alone;
that is preferable to false success for a different write.

This correction does **not** provide atomic publication across
private Slice LIST and inode attributes. The separate question of
remote visibility after a failed in-EOF Flush is now expressly accepted
by the owner. Private records can be visible despite Flush failure;
what must not happen is falsely acknowledging Flush, dropping Dirty
candidate ownership or replaying the same append as a duplicate. This
P1 decision does not affect the P2 correction.
