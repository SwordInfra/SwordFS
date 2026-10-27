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

## Status

First full-Slice RED evidence exists in #512 already. Focused independent RED
and precise fix are pending; do not claim resolution.

## Current first targeted comparison — CI #40446185097

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

The correctness fix remains pending root-cause analysis; this new test gate
must **not** be waived to merge or to reclassify internal failures as PASS.

## First focused production improvement: bounded packing of sparse intervals

After establishing both forms of RED (full Slice timeout and the real
hidden-writeback-error artifact rejected by the new verifier), the source
path reveals that disjoint 8-byte page-strided writes were frozen into one
physical Slice object **per distinct interval** even though they share one
logical Chunk. At 256 MiB with 4 KiB stride, one holetest writer can issue
roughly 65,536 separate pwrite calls. The existing `active_bytes_` cap (4 MiB)
counts payload bytes, not interval count; this can explode S3 PUTs and Redis
allocation/record pressure without any logical increase in user data.

The first fix **packs multiple disjoint frozen candidates into one immutable
physical object of at most 4 MiB**, retaining separate ordered SliceRecords
with distinct `pos`, `off` and `len` but shared SliceID and the physical
object's total `size`. It must not zero-fill unwritten gaps or accidentally
hide previous revisions. The frozen candidates remain stable during PUT;
truncation may clip visibility and never promotes an invisible candidate;
failed/ambiguous PUT retries use a new physical identity. Successful
publication still occurs per logical record and remains behind FileMetadata
EOF preconditions. The change does not optimize all Redis record append
round trips or automatically resolve inode WATCH contention, which must be
verified with CI rather than assumed.

**TDD:** new `SparseSmallWritesSharePhysicalObjectWithoutShadowingGaps`
uses 13 distinct 2-byte writes over a previously committed 100-byte
nonzero file. Old code creates 13 PUTs, new expectation is **one PUT**,
14 ordered logical records including initial history, all 13 sharing one
physical ID with compact offsets. A cold read checks every untouched
gap remains the exact original byte, not fabricated zero. Existing sparse
correctness assertions are updated to expect one *shared* immutable object
instead of one per sparse range. Preserve tests for partial record commit,
OutcomeUnknown, in-flight Put, 4-MiB limit and in-order logical records.

Formal full CI must validate all C++ UT and Codecov patch, plus strict
actual `generic/346` COW/Slice mounted logs and eventual full #512 fstests.
