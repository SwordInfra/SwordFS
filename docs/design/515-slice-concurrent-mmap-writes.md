# #515 — Restore Slice progress under mmap/regular-write/truncate races

## Observed failure and authority

The pinned upstream fstests `generic/346` exercises an independent pair of
`mmap` and ordinary writer threads using `holetest` on the same file, at
1 MiB, 16 MiB and 256 MiB sizes, with zero/`posix_fallocate`/`ftruncate`
subcases. It is part of SwordFS's independently proven COW-supported contract.
In #512 CI #40442989636, exactly this testcase timed out in Slice after
600s; 15 preceding tests in the Slice supported-5 shard passed and the
same-run COW supported-5 shard passed fully. The timed-out testcase left a
live FUSE daemon, aborting 13 later tests. All 116 other executed
COW-supported cases passed in Slice; 29 were not observed due to the abort.

Real Slice logs during the timeout recorded `Redis transaction retry limit
exceeded` and `Slice publication lost too many EOF/attachment races`. The
strong evidence is repeated failed commits and a stall; the *root cause*
remains to be established. Higher resident memory during the 256-MiB stress
(~1.0 GiB RSS) is observed but not proven leaked.

## Invariants / strategy

- Preserve real pinned `generic/346` unchanged: it is an externally meaningful
  POSIX concurrency contract, not an internal attempt-count test.
- Establish **formal CI RED on the exact case**, using a fresh Slice-formatted
  Redis/MinIO/FUSE setup and the same pinned test binary/toolchain as main CI.
  Use a separate COW-format control on the same runner and publish independent
  raw `holetest`, daemon and timeout evidence. The default production mechanism
  stays COW.
- Do not treat ambiguous Redis EXEC as retryable or disable EOF/attachment
  preconditions. Repeated WATCH conflicts, immutable Slice record append
  identity and safe publication need direct diagnostic analysis before changing
  production logic. Redis/metadata operations must remain on the thread domain.
- No longer timeout, indiscriminate Redis retry-limit inflation, testcase
  omission/waiver, or unrelated Slice GC/compaction/backpressure changes.
- After a well-supported root cause and test-first RED, implement the smallest
  compatible fix and prove GREEN with targeted real FUSE CI, the affected
  supported-5 shard and full #512 Slice matrix. Keep COW semantics validated;
  changed production C++ files must clear per-file patch coverage >90%.

## Reproduction-first CI

A separately labeled `slice-346-regression-ci` job reuses the *same-run*
immutable Release binary, pinned upstream fstests/liburing bundle and MinIO
tools. It runs the same upstream `generic/346` once with COW and once with
Slice on fresh backends/volumes, records their own independent raw evidence,
and requires actual `chunk_type` metadata plus real XUnit identity and PASS
for a successful run. Failure stays visible and blocks the opt-in job;
regular required COW and other CI checks remain unchanged.

## Unified Slice with 1 MiB dirty-window cache

There is **one** Slice Chunk mechanism. Its 64 MiB logical Chunk, immutable
S3 Slice objects, ordered Redis SliceRecord history, LWW publication, and
EOF/truncate authority are unchanged. The 1 MiB Window is **only a client
dirty-write cache coalescing boundary**: disjoint small writes accepted in
the same window can form one contiguous Slice after published bytes in their
gaps have been read and overlaid with the accepted dirty bytes. The
contiguous Slice is published through the existing ordinary record path.

For one contiguous write, including a dense multi-MiB write, use an ordinary
Slice without artificially splitting it at Window boundaries. A single
small interval similarly does not need a 1 MiB S3 hydration. Do not extend
EOF to the end of a cache window: only accepted dirty bytes can do so.

**No alternate implementation survives:** remove all opt-in Legacy, packed
Sparse, fixed-window, and span-window strategy selection in the final source
and CI. The earlier four-arm benchmark is historical research, not a runtime
configuration or new metadata model. The single Slice mode is now the target
of normal tests and the strict unchanged `generic/346` COW/Slice CI comparison.

Multi-mount concurrent overlapping windows follow the agreed LWW publication
model. Do not promise preservation of concurrent disjoint byte updates when
a stale full/covering-window image is published by another mount. An
uncertain Redis append or failed S3 PUT still must fail closed and retain
dirty bytes for the supported retry protocol. Per-window size is not a new
durable identifier, lock, or metadata schema.

**Accepted partial-Flush LWW boundary (2026-10-28):** A prepared Window RMW
candidate contains both accepted local writes and committed bytes read to
hydrate its gaps. If an earlier window publishes successfully but this
candidate's publication fails, the retained local pending view may continue
to expose that hydrated image over the entire coalesced interval. A later
write by another mount into one of those gaps is not guaranteed to appear in
this mount's local reads before retry, and an eventual successful publication
may overwrite it under Window-level LWW. This behavior is intentional;
there is **no per-byte cross-mount merge or immediate invalidation contract**.
This acceptance does **not** permit silent I/O errors, acknowledging a failed
or uncertain publication as successful, or violating EOF/truncate authority.

The experiment-only S3/Redis timing counters were removed: their shutdown
snapshots missed the busy daemon and incorrectly reported zero S3 time.
CI retains Redis `INFO commandstats` deltas, authoritative testcase phase
times, and raw FUSE daemon logs. Do not infer cumulative S3/Redis service
time percentages from unavailable instrumentation.

### Read-only transaction amplification diagnosis and controlled change

CI #40474783429 recorded ~145k Redis EXEC calls but only 801 RPUSHX
operations in the 1 MiB RMW arms. The bulk of WATCH/EXEC is associated with
repeated `ReadFileChunkSnapshot` calls, even for absent/zero-filled Chunk
positions. This is not evidence of 145k write transactions or WATCH failures.

The #515 follow-up changes only `ReadFileChunkSnapshot` to a read-only
server-atomic `EVAL_RO` path: a first plain GET obtains the opaque inode for
local parsing/EOF-boundary derivation; a single atomic script compares that
exact encoded inode and returns the requested ChunkRef plus the EOF boundary
ChunkRef. If the inode changes between GET and script execution, retry with
a fresh inode. This preserves the old coherent-snapshot contract **without
WATCH/MULTI/EXEC**, including matching inode size and boundary identity.
Malformed ChunkIDs, incorrect Redis types and Redis failures must fail closed.

Do **not** permanently negative-cache absent Chunks without a cross-mount
invalidation/version protocol: another Mount could attach new data. A cache
experiment would be separate from the safe read-only protocol change.

## Historical baseline evidence — CI #40446185097

The first formal opt-in COW/Slice control **both passed upstream XUnit**:
`generic/346` took **25s on COW, 453s on Slice** (18.12x on the same runner)
with a 600s per-case limit. The raw Slice `swordfs-scratch.log` nevertheless
contained seven `FileReadWriter::Flush chunk FAILED` events and one
`local RELEASE failed`, including Redis optimistic transaction retry
exhaustion and `Slice publication lost too many EOF/attachment races`.
The COW daemon logs were clean. Therefore the originally green job is **not
sufficient evidence of correct Slice durability or reliable performance**.
No blanket throughput threshold is set from a single comparison.

### Stronger TDD gate

The focused validator now independently requires **clean FUSE writeback
logs** from both TEST and SCRATCH, in addition to authoritative XUnit,
mechanism metadata, and upstream harness success. A new tooling test first
**failed 3/5** against the old validator (accepting Flush errors,
RELEASE errors and missing logs); after the change it must be GREEN. The
same downloaded *real* artifact from green CI #40446185097 must fail the
new semantic gate on Slice, proving the diagnostic is neither hypothetical
nor a spurious performance benchmark. The real, still unmodified
`generic/346` will run again in formal CI on the new head to capture
consumer-visible PASS versus internal writeback RED.

The later unified implementation passed the strengthened validator on
[CI #40524918284](https://github.com/SwordInfra/SwordFS/actions/runs/40524918284).
The gate must **not** be waived or treat internal writeback errors as PASS.

## Superseded experiment: packed sparse physical objects

The initial investigation tried packing disjoint frozen intervals into a
shared immutable physical object, recording separate logical SliceRecords
with differing `off` values. It reduced physical PUT count, but did not
eliminate the large Redis record and write-side transaction amplification.
This was an **experiment only**, represented by the historical
`caf811a` commit; its implementation, selection controls, and packing-specific
TDD test were removed before the final PR revision. It is not a second
production Slice format or a mechanism to preserve for beta compatibility.

The surviving implementation instead coalesces multiple dirty intervals
within the same 1 MiB cache window into **one contiguous candidate and one
ordinary immutable Slice object/record**. Existing committed bytes inside
the covering interval are fetched before the dirty writes are overlaid;
`eof_len` tracks only the extent of locally accepted writes. Single contiguous
writes are left as regular Slice candidates. `WindowCacheCoalescesSparseDirtyIntervalsWithoutShadowingGaps`
and the partial-failure, truncate-boundary, committed-hydration-error, and
in-flight-PUT tests describe the final behavior. The targeted full CI must
still validate strict `generic/346` COW/Slice mounted logs; the full #512
Slice-primary rollout is a separate subsequent task.
