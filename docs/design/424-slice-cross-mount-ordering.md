# #424 — Cross-mount Slice consistency: final beta contract

Status: **Design decision accepted (2026-10-20)**. Scope: investigation and
design, **no production implementation** in this task. Parent: #201.
Design PR: [#498](https://github.com/SwordInfra/SwordFS/pull/498).
Implementation: [#271](https://github.com/SwordInfra/SwordFS/issues/271)
(Write/Flush), [#272](https://github.com/SwordInfra/SwordFS/issues/272)
(Truncate/Grow), [#273](https://github.com/SwordInfra/SwordFS/issues/273)
(GC), [#274](https://github.com/SwordInfra/SwordFS/issues/274)
(compaction), [#275](https://github.com/SwordInfra/SwordFS/issues/275)
(activation).

## Final decision: keep the existing indexing architecture

Do **not** add a per-inode XADD authority journal, distributed file lock,
universal two-phase transaction, Lua boundary script, live LIST key rename,
or O(history) snapshot copy to make every cross-mount EOF race atomic.

Preserve the existing separation:

```text
FileMetadata (IMetaEngine): inode EOF/attributes, ChunkIndex -> ChunkID
SliceChunkMetadata (#484): ChunkID -> ordered SliceRecord Redis LIST
Storage (#485): frozen immutable objects slice/<SliceID>
```

For **ordinary in-EOF writes** on an already attached ChunkID, native
Redis `RPUSHX` provides record ordering. Disjoint ranges coexist; for
overlapping ranges, the later LIST entry overlays earlier bytes. No
per-write Redis WATCH, global epoch or cross-client lock is required
merely to maintain this behavior.

For **first attachment**, allocate a fresh ChunkID, physically upload the
immutable object, persist a nonempty first-record private LIST, then
conditionally publish the ChunkIndex -> ChunkID mapping in FileMetadata.
Failed preparation/losing attachment may leave unreferenced private
metadata and objects; mapped missing/invalid LISTs must fail as errors,
not silently become sparse holes.

The beta filesystem does **not** promise full-file serializability,
cross-mount visibility of unflushed Dirty Read, or atomic publication
of a multi-chunk write. Flush/fsync can succeed only after its intended
actual metadata publication succeeds, subject to the two explicitly
documented cross-domain concurrent-failure exceptions below.

## Explicitly accepted beta limitations: only two proven races

The owner accepted *deferring* strict atomicity between independently
updated FileMetadata EOF and Slice-private records for these two cases.
**These are genuine data-integrity risks**, not merely leaked garbage.
Do not assert that SwordFS protects acknowledged data against these
exceptions; do not silently reinterpret them as successful publication.

1. **Unconfirmed EOF-extending record can appear.** A appends a private
   [8,12) SliceRecord to a Chunk with EOF 8, then its inode EOF finalize
   fails while another mount grows EOF to 10. Bytes [8,10) of A's
   **unconfirmed** record are now readable; a later grow to 12 may reveal
   the rest. These bytes need not belong to any successful Flush.
2. **Failed stale zero-cover can obscure confirmed data.** A starts from
   EOF 4. B successfully writes [4,8) and extends EOF to 8. A appends
   a zero-cover for [4,8) after B's record, then its separate inode
   EOF CAS fails. LIST last-wins gives A's **failed** cover precedence,
   concealing B's acknowledged bytes.

Both were reproduced on an isolated, disposable Redis 7 service using
the repository's actual 24-byte SliceRecord format and two Redis
connections, not actual SwordFS mounted clients. See reproducible
`scripts/testing/slice-publication-consistency-probe.py`.

These two failures are **not** reasons to block #271/#272 in beta;
they remain known limitations to disclose in #275's activation docs.
Do not generalize this waiver to corruption of mapped indexes,
loss of normal non-racing writes, false ordinary Flush success, unsafe
GC reclamation, or broken sequential Truncate/Grow semantics.

## Required correctness outside the two exceptions

- **Publish object bytes before metadata.** A physical Put does not
  itself publish a SliceRecord. The #271 frozen-candidate publisher
  commits actual ordered records, inode attrs/EOF where applicable,
  and retires only generations with confirmed metadata success.
  On an ordinary failed Flush, local Dirty Read retains not-yet-
  published bytes. An unknown Redis reply is not proof of rollback.
- **Sequential truncate/grow must not resurrect removed bytes.** After
  an ordinary completed shrink, extending EOF or sparse-writing later
  must reveal zeroes in the discarded gap. A retained boundary ChunkID
  must have persistent, mechanism-private sanitation semantics;
  mount-local `SliceChunk::CaptureBoundary` is not a cross-mount
  persistent zero-cover protocol. #272 owns the concrete primitive.
- **New attachments must be readable.** CreateIndex writes a real
  first SliceRecord before FileMetadata attaches the fresh ChunkID.
  A competing first writer must rebase/retry or fail without silently
  discarding an already confirmed attached record.
- **Normal concurrency and reads.** Same-ID nonconflicting LIST
  appends coexist; overlap resolves by Redis append order. An old
  candidate that genuinely appends **after** a concurrent shrink/grow
  can be a valid later overlapping write; numeric EOF ABA alone does
  not justify a new permanent generation field. A mapped index
  with missing/corrupt data is an error; read-side refresh for
  concurrent legitimate attachment change stays within #271.
- **Conservative cleanup.** Unreferenced Slice objects/indexes may
  leak. #273 must never delete reachable or reader-pinned data;
  #274 must protect accepted records while compacting. Neither
  depends on completion of a new global #424 transaction layer.

The implementation must not claim **stronger** semantics than it
actually demonstrates. #275 remains gated by #271, #272, #273, #274
and integrated test results, but **not** by implementing failure
atomicity for the two accepted races.

## Evaluated but not adopted: Slice-private valid_length

A `SliceChunkMetadata.valid_length` marker would allow readers to
clip record overlays to
`min(FileMetadata EOF within chunk, private valid_length)` and emit
zeroes above the private prefix. This can improve a *pure grow* after
a failed out-of-range Slice append if the private length stays
unchanged. It is a viable *optional* #272 implementation detail for
ordinary sequential shrink/grow, not a requirement of #424.

**One byte length is not a per-SliceRecord commit boundary.**
Example A fails to finalize its private [8,12) record while length=8;
B legitimately writes [10,12) and advances the length to 12.
A's previously unconfirmed [8,10) data reappears. Likewise a failed
zero-cover for [4,8) can obscure B's confirmed [4,8) data while the
private length already equals 8. This was independently checked with
a small deterministic overlay model.

Unlike `COWChunkHead{revision,size}` which binds an immutable object
revision and visible prefix under one CAS, a Slice LIST remains
independently mutable. Therefore **do not add valid_length while
claiming it solves the above two races**. #272 may select or reject
it based on *normal sequential truncate/grow correctness* and
Redis read/write RTT, without a new version framework.

## Direct JuiceFS peer comparison

Checked upstream `pkg/meta/redis.go` (`doWrite`,
`doTruncate`, `doCompactChunk`) and `pkg/vfs/writer.go`
(`chunkWriter.commitThread`, `Flush`), at inspected revision
`29c60bbbeea4`.

- JuiceFS uses a Redis LIST of Slice references per file/chunk; frozen
  object upload is separate from metadata commitment.
- Its `doWrite` uses an inode-key WATCH transaction to queue a Slice
  `RPUSH` and inode attribute/EOF SET. Its `doTruncate` likewise
  couples EOF with zero-cover records in one scoped transaction.
  It does **not** use a per-file authority Stream or global
  transaction across object storage.
- Compaction verifies the old LIST prefix before replacing it in a
  targeted Redis transaction. The public consistency model focuses
  on close-to-open, not instantaneous cross-mount Dirty Read.

SwordFS deliberately separates FileMetadata and SliceChunkMetadata
consistency domains (#394). Our **accepted beta exceptions** are
exactly where a small JuiceFS-style publication would be stronger.
We have explicitly decided not to require this extra coupling yet,
rather than claim the two designs are identical.

Source: [JuiceFS Redis metadata](https://github.com/juicedata/juicefs/blob/main/pkg/meta/redis.go),
[JuiceFS writer](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/writer.go).

## Evidence and downstream verification ownership

`scripts/testing/slice-publication-consistency-probe.py` was executed
against a loopback-only disposable Redis 7 instance, stopped and
removed afterward. The actual results were:

| Experiment | Result | What it proves |
| --- | --- | --- |
| Disjoint/overlapping normal in-EOF RPUSHX | PASS | Existing ordered LIST primitive works |
| Separate RPUSHX then failed EOF CAS, B grow | REPRODUCED | Unconfirmed tail can become visible |
| Separate stale zero-cover then failed EOF CAS | REPRODUCED | Failed cover can mask B's success |
| Scoped inode WATCH + queued RPUSH/EOF | PASS | Normal WATCH conflicts abort together |
| Sequential shrink+cover followed by grow | PASS | Zero-filled tail possible without new index |
| Late append around shrink/grow | OBSERVED | EOF ABA alone is not necessarily a defect |

The WATCH proof **does not** establish safety under EXEC
per-command runtime errors or ambiguous network outcomes. The
probe **does not** invoke SwordFS's unfinished Slice publisher,
a FUSE mount, or full integration CI.

**#424 is complete as a design decision and risk acceptance, not
as implementation.** #271 must implement and test real Write/Flush;
#272 normal persistent zero-cover and grow; #273 GC; #274 compaction;
#275 integrated E2E and user-visible beta warning. Do not reopen
#424 as a prerequisite unless a *new, validated* correctness defect
falls outside the explicit waiver.
