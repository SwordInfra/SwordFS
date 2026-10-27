# #512 — Promote Slice to the primary FUSE/POSIX test mechanism after parity

## Baseline and rationale

SwordFS's production format default remains `cow`, but #508 proved a real
explicitly formatted Slice FUSE volume (171/171 E2E) and #510 proved complete
pinned Slice pjdfstest for the currently COW-supported assertions (8,770/8,770)
and only **four** Slice fstests smoke cases. Therefore changing the default
CI conformance mechanism immediately would be an unjustified compatibility
claim. Full fstests coverage and mechanism-specific failure evidence come first.

## Contract and protected invariants

1. **Separate the test default from the production default.** After proving
   Slice's currently supported behavior, make the primary *CI* E2E/pjdfstest/
   fstests run on Slice. `swordfs format` stays COW by default pending #275.
2. During discovery, execute the **entire pinned COW fstests selection** on
   Slice, across the identical `plan.py` partitions. TEST and SCRATCH volumes
   must both format with immutable `chunk_slice` type. Every selected testcase
   is either executed or explicitly deferred by the existing pinned harness
   rules; do not accept missing cases, shard artifacts or XUnit reports.
3. Report Slice evidence **separately**, with an independent Slice support/gap
   baseline, raw XUnit, harness status and a typed `chunk_type=chunk_slice`
   environment marker. Never read COW historical FAIL evidence as Slice facts,
   never change COW's `supported.txt` or `known-gaps.tsv` to hide a Slice
   regression. The current COW-supported set is the **minimum pass contract**.
4. Keep all existing required COW checks green during discovery, do not double
   the entire expensive CI permanently. Use an explicitly opt-in PR label for
   the parallel Slice full matrix while measuring failures, CI costs and
   sharding. After successful evidence, move the primary pipeline to Slice and
   keep bounded independent COW mechanism-specific regression tests.
5. An actual Slice correctness failure calls for test-first reproduction and
   a focused fix, not a conformance waiver. Limit #512 to CI/test correctness;
   physical Slice GC (#273), compaction (#274), mount memory (#202) and
   production default/performance acceptance (#275) retain their separate
   ownership. The documented #424 beta multi-mount LWW exceptions remain.

## Stage A: opt-in full-Slice parity probe

Add `slice-fstests-full-shard` as an optional full matrix with the same ten
pinned shard IDs and per-shard timeout profile as the required COW jobs.
Use the exact same `plan.py` output and shared immutable Release and fstests
artifacts. Each shard formats **both** TEST and SCRATCH Slice volumes and
uploads an artifact with a Slice-only prefix. A separate aggregation job
validates completeness of all shard artifacts, selected testcase identities,
XUnit results, and `chunk_type=chunk_slice` per environment. Then run the
existing strict classifier with the original COW supported set as the
minimum supported-pass expectation and **no preapproved Slice gaps**; any
other failed/NOTRUN case is reported as unclassified and remains a blocker
until observed, investigated and adjudicated with independent evidence.

This opt-in experiment is intentionally allowed to fail on real untriaged
Slice POSIX cases. A failure is evidence for subsequent remediation, not a
reason to auto-promote COW's historical failure classifications to Slice.

## Stage B: promotion conditions

1. A completed all-shard run proves zero missing/duplicate executed cases,
   COW-supported testcases remain supported by Slice, and all Slice-specific
   non-PASS observations are independently classified without broad waivers.
2. Full Slice E2E/pjdfstest/fstests becomes primary CI and published status
   only with mechanism-tagged artifacts; preserve independent COW targeted
   test path while removing redundant duplicate full COW runs.
3. Formal all-required CI, Codecov per-production-file patch gate, review,
   PR squash merge and **main CI** all pass. No test default cutover is
   implied by the Stage A full-matrix probe alone.

## Verification / review

- Run `plan.py --summary-json`, test existing pinned complete coverage,
  inspect source diff, YAML/pre-commit and status/report metadata.
- Formal GitHub CI on an explicitly labeled Draft PR is the only real
  FUSE/Redis/MinIO/pinned-upstream execution authority.
- Avoid adding native compile workloads to the local development server.
