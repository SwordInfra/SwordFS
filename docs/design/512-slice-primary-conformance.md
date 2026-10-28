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

The CI workflow subscribes to the `pull_request.labeled` event explicitly
as well as ordinary open/synchronize/reopen events. Otherwise applying the
`slice-full-matrix-ci` opt-in label alone does not schedule a new CI run,
and any already queued run remains anchored to its original event label
snapshot. With that event type, reapplying the label starts a fresh,
mechanism-qualified full matrix on the *current* exact PR head. The
required COW matrix is never skipped by this opt-in event.

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

## Stage A evidence review: Slice-only pinned compatibility baseline

The exact first full Slice CI run **#40442989636**, commit
`bb39c58f8c03f7dca8265b78320f6779195ab2a3`, on the pinned upstream
`a370dcbed43563f0462801e889e0eceb93c7cfad` produced 615 actual XUnit
results under `chunk_type=chunk_slice` from ten separately tagged shards.
One shard aborted at `generic/346`, leaving 29 supported tests without XUnit.
The 116 completed supported tests passed. Exactly one other selected test is
pinned and deferred. None of these missing cases is converted into a gap.

We **independently compared every one of the 499 Slice-observed FAIL/NOTRUN
identities and normalized XUnit messages** against the prior FUSE test-model
hypotheses before producing `conformance/slice/known-gaps.tsv`:

| Root cause category | Actual Slice XUnit outcomes | Review basis |
| --- | ---: | --- |
| `upstream_not_applicable` | 403 | 42 grouped FUSE-protocol/hosted-block-test prerequisites and environment assumptions. Includes lack of local block device, local mkfs/quotas, reflink/dedupe probes, forced shutdown, physical extent layout, and nonlocal kernel facilities. These conditions are independent of volume Chunk implementation. |
| `known_unsupported` | 87 | Reviewed 12 distinct common VFS/FUSE missing capabilities: fallocate modes, trusted xattrs, RENAME_WHITEOUT, inode flags, FUSE ACL kill-SGID, FUSE export handles, copy_file_range, labels, delegations, caching. Issue references (#255/#406) and implementation ownership remain independent of COW versus Slice. |
| `environment` | 8 | Hosted test runner/core dump, logging device, scsi_debug and pinned toolchain prerequisites; not Slice data-plane failure evidence. |
| `upstream_test_defect` | 1 | `generic/591` upstream splice accounting behavior: the independently observed Slice `.out.bad` is identical to the known upstream repro. |
| **Total** | **499** | **482 NOTRUN, 17 FAIL** on the actual Slice data, no newly invented exclusions. |

All **15 independently observed** `.out.bad`/`.mountfail` failures were
canonicalized from the *Slice* raw shard artifacts, not copied from the COW
checked-in files. Their canonical contents also independently matched the
existing reference evidence byte-for-byte; two remaining FAIL cases are
hosted SIGXFSZ core outcomes without a detailed XUnit evidence path. These
are proposed documented baseline expectations for **the present FUSE model**,
not evidence of Slice writeback correctness or a reason to excuse newly
failing supported tests. Every entry, reason, issue and detailed output is
checked by the same fail-closed strict classifier as primary COW CI.

Once the Slice-specific manifest and FAIL evidence are used, **the original
incomplete first run reclassifies from 529 blockers to exactly 30
`INFRASTRUCTURE` blockers** (one aggregate harness status plus 29 missing
supported cases). The number of passing supported tests stays **116/145**,
not 145/145. There are no mismatched expected results, messages, or detailed
evidence. This offline replay is an observation-classification test, **not**
completion of the required full Slice fstests parity.

**Provenance:** GitHub Actions run #40442989636; immutable `slice-fstests-full`
classification and ten `slice-fstests-full-*` raw artifacts. For this
snapshot `conformance/slice/known-gaps.tsv` canonical SHA-256 is
`18fe2af3d485a65f1a6d453b98924702caa72b649cca2389e7865ab74ea8481a`.
Future runs revalidate exact test outcomes and *detailed failure bytes*, rather
than accepting a broad `FAIL`/`NOTRUN` category or a generic skip waiver.
Reviewers should explicitly check the non-FUSE-specific gap rationale before
promoting the proposed manifest to authoritative primary-CI status.

## Stage B cutover design (prepared, not activated)

A single cohesive final change should **replace the primary CI test mechanism**
only after #515 has merged and its post-merge `main` CI is green, and a clean
10-shard *Slice* run has proven all 145 existing COW-supported tests PASS,
strict slice-specific known gaps matched, zero missing results, and no
unexpected internal writeback, FUSE unmount, or artifact identity failure.

1. **FUSE E2E:** in required Release `e2e-test`, explicitly set
   `SWORDFS_E2E_CHUNK_TYPE=chunk_slice` (including the intentional-failure
   diagnostic environment), and pin format evidence to Slice. Retain the
   independent Slice Debug unit/E2E correctness runs where they add failure
   coverage; replace, do not permanently double, the standard main E2E
   matrix. Preserve dedicated `tests/unittests/chunk/cow/` and a bounded
   mounted COW-format smoke job to detect mechanism-specific regressions.
2. **pjdfstest:** on the required `pjdfstest-conformance` runner explicitly
   set `SWORDFS_CONFORMANCE_CHUNK_TYPE=chunk_slice`; preserve complete pinned
   upstream assertion population and all existing strict `supported.txt`
   expectations. Qualify `result.json` environment and published metadata by
   the actual Slice volume type. COW-specific tests remain separate.
3. **fstests:** run the existing ten-shard deterministic complete pinned plan
   as the required primary Slice CI instead of launching two full matrices;
   use `conformance/slice/known-gaps.tsv` and checked-in Slice-only FAIL
   evidence. Preserve all strict previous supported-count and pinned
   selected-set checks. Retain a bounded COW regression, not a second
   full COW matrix. It must be impossible for the aggregator to combine
   different mechanism outputs or to treat missing TEST/SCRATCH logs as pass.
4. **Status publishing:** keep old COW status/history artifacts distinguishable
   from the new authoritative Slice results. Explicitly tag both
   `latest.json`/`history.json` and Markdown with `chunk_type`, with
   mechanism-qualified history grouping or a fresh Slice-specific status
   branch; never render COW historical supported percentages as a continuous
   Slice trend. Update README compatibility links and any status consumers
   together in the cutover PR. A failed, incomplete or misidentified primary
   Slice matrix must fail closed and publish failure evidence, not a success
   status inferred from XUnit PASS alone.
5. **Promotion gates:** Debug/Release, coverage, pin-identity checks, complete
   genuine Slice full fstests CI, targeted COW regression, independent review,
   squash merge, and post-merge main CI. `swordfs format` production default
   **stays COW** until the distinct #275 activation criteria are met.

No Stage B job defaults, publishers or README links should change on the
Stage A discovery-only Draft PR before the strict parity gate is met.

## Post-#515 full probe: why 145/145 XUnit PASS is not the cutover gate

[Full CI #40616350589](https://github.com/SwordInfra/SwordFS/actions/runs/40616350589)
reused the merged `#515` cache/metadata implementation, pinned test harness,
verified Release and independent reviewed Slice expectations. It delivered
**10/10 complete typed Slice shards, 644/644 executed cases, 145/145 supported
XUnit PASS and 499 exact known gaps** (403 upstream FUSE-not-applicable, 87
VFS-known-unsupported, 8 environment, 1 upstream defect), 1 deferred and
**0 blocking XUnit/aggregate classifications**. `generic/346` completed in
55 seconds, rather than timing out at 600 seconds as before #515. The
supported population is now fully observed without truncation.

However the raw `slice-fstests-full-supported-5` scratch daemon log reports
`FileReadWriter::Flush chunk FAILED: ino=290 chunk=2 — Slice publication lost
too many EOF/attachment races` at 2026-10-28 01:51:25 UTC **during the
same 55-second generic/346** (scratch daemon generation 46, running from
01:50:39 to 01:51:33 UTC). The upstream checker does not reliably propagate
async pwrite/RELEASE writeback failures, so this definite error means the
XUnit-based compatibility classification alone **cannot establish durability**.
The child #515 owner received the artifact and must remediate/validate it;
#512 does not modify #515's C++ code.

### Fail-closed asynchronous writeback contract

Stage A adds mechanism-qualified **per-test writeback audit** for Slice full
fstests. Before each original upstream testcase, save the current byte offsets
of both append-only FUSE daemon logs; after the harness has completed and its
FUSE mounts have been isolated, scan only bytes produced by that testcase for
`FileReadWriter::Flush chunk FAILED` and `local RELEASE failed`. Any unexpected
line writes `raw/results/<suite>/<id>.writebackfail` with exact diagnostic
context, and the aggregate classifier marks that exact testcase
`WRITEBACK_FAILURE` (blocking) **even if upstream XUnit says PASS** or a known
baseline gap otherwise matches. The new tool is tested against the real
previous CI supported-5 artifact and correctly detects the single hidden
failure. The audit fails closed on missing/truncated logs or invalid IDs.

One pinned testcase, `generic/079`, deliberately checks immutable and
append-only inode flags. Its FUSE log includes expected policy-denial
`finalize chunk write rejected by inode policy`, with upstream PASS and
corresponding intentional permission-denial operations in pinned
`tests/generic/079`. Only this **exact reason in this exact testcase** is
excluded; never suppress a retry-exhaustion, EOF/attachment, S3 or unrelated
writeback failure, including those emitted by `generic/079` itself. The rest
of the 145 supported tests remain subject to strict per-case checks.

The initial 145/145 *XUnit* pass is valuable stage-A coverage evidence, but
promotion to primary tests is blocked until a formal 10-shard run also passes
this new asynchronous-writeback gate with **zero** unexpected errors, alongside
all ordinary project gates. The production COW/Slice default is unchanged.
