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
