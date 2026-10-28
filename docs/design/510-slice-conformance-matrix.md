# #510 — Independent Slice POSIX conformance evidence

> **Current default (superseding historical stages below):** #518 makes
> newly formatted volumes and shared conformance scripts default to
> `chunk_slice`; COW remains an explicit choice (`--chunk-type cow`) and
> existing volumes mount using their persisted mechanism. The earlier COW
> defaults and opt-in-only instructions below document their historical
> stage and are not current configuration guidance. Activation safety gates
> remain in [#275](https://github.com/SwordInfra/SwordFS/issues/275).

## Baseline and problem

PR #509 allows explicitly formatted Slice volumes and passes the full 171-case
mounted FUSE E2E suite. Existing pjdfstest/fstests jobs **still format COW**.
Their green status is *not* evidence that Slice supports the same POSIX contract.
The initial goal is separate, reproducible Slice conformance experiments while
preserving every established COW required check and its published baselines.

## Invariants

1. Conformance runs default to **the exact existing COW format arguments**.
   The new `SWORDFS_CONFORMANCE_CHUNK_TYPE=chunk_slice` opt-in must be strictly
   whitelisted before any work and affect only the `swordfs format` command,
   not a mount-level override. The fstests TEST and SCRATCH volumes must share
   the same persistently configured chunk type.
2. Record `chunk_type` in each run's `environment.json`. COW public reports,
   baseline allowlists and expected job names remain unchanged. Slice results
   belong to separately named artifacts/jobs; never replace or silently
   overwrite COW baseline outputs.
3. Use the existing pinned Release binary, Redis, MinIO, FUSE and upstream
   test toolchains. Report executed counts and actual failures, including
   infrastructure errors; no inferred or artificially forced Slice success.
4. Slice GC (#273), compaction (#274), mount-wide memory policy (#202),
   full activation (#275) and the explicitly accepted #424 cross-mount LWW
   semantics are unchanged. A conformance experiment is not proof of
   production readiness.

## Planned tests and acceptance

- Simple shell/CI tooling tests for default COW, explicit COW, Slice option,
  invalid/injected option and missing mechanisms; always refuse unknown types.
- Actual pinned pjdfstest run on an opt-in Slice volume, saving raw evidence,
  machine-readable status, and a mechanism-qualified summary/artifact.
- Actual pinned selected fstests run on Slice TEST and SCRATCH volumes with
  mechanism-qualified environment/evidence. Expand selected cases based on
  observed per-test pass/fail, not on the COW report's support ratio.
- Keep baseline COW jobs strict. Additional Slice experiment may expose
  regressions, but must not reclassify COW baselines or silently excuse Slice
  test failures.

## Change structure

Shared shell helper `scripts/conformance/chunk-type.sh` owns one checked
mechanism selector and an optional CLI flag array; both existing harnesses
source it before side effects. Only their format/evidence paths consume the
selector. Test execution, mount options, backend teardown and CI baseline
classifiers remain untouched. Introduce Slice CI experiment on existing pinned
binaries/tools with separate status.

## Opt-in CI first experiment

`.github/workflows/ci.yml` adds **one** `slice-posix-experiment (Release)`
job, activated for PRs with the `slice-conformance-ci` label or explicit
workflow dispatch. This is separate from the existing required COW jobs. The
experiment reuses the same-run verified Release binary, pinned MinIO artifacts
and pinned fstests/liburing artifact; it installs runtime requirements once
for both upstream suites on the same runner, sequentially.

The experimental runner executes the complete pinned pjdfstest TAP suite on
one explicitly Slice-formatted volume and compares it in strict mode against
the currently supported POSIX assertions. It then executes the deliberately
bounded `conformance/slice/fstests-smoke.txt` on **two fresh Slice volumes**
(TEST/SCRATCH) and compares those expected-good cases using strict mode. An
unexpected failure in either harness or classifier makes the **Slice job**
fail, while existing COW baselines and their `supported.txt` are untouched.
Results are uploaded to the unique `slice-posix-experiment` artifact, including
mechanism-qualified environment metadata and raw output, so an initial failure
is actionable rather than silently reclassified as known support.

This is an initial Slice-versus-COW semantic probe, **not** the final full
Slice fstests conformance gate. Expand smoke cases based on observed evidence
before switching Slice to the default mechanism.

## Reused producer artifacts across failed-job retries

A GitHub Actions **failed-jobs-only rerun** is allowed to reuse verified
artifacts from successful producer jobs in the previous attempt. The old
consumer command compared the artifact's embedded `run_attempt` to the
*consumer's* `${GITHUB_RUN_ATTEMPT}`, which incorrectly rejected a valid
producer-attempt-1 artifact when consumers retried on attempt 2. The first
full Slice-fstests CI attempt exposed exactly that failure: original Release
APT provisioning timed out, then a retry succeeded in rebuilding Release but
all fstests tool consumers rejected the still-successful attempt-1 toolchain.

Both producer jobs (`build-release-core` and `build-fstests-tools`) now export
their `archive_attempt` alongside the content digest. **Every consumer**
verifies the source SHA, workflow run ID, producer attempt, pinned version
metadata and digest; it never uses its own retry number to assert producer
provenance. If the attempt/digest outputs are missing, prerequisite checks fail
closed. This does not permit cross-run or cross-source artifacts or disable
any integrity check, and it restores the normal GitHub failed-only rerun path
for both COW and Slice conformance.

## First real Slice POSIX smoke evidence (2026-10-27)

Full [CI #40429424432](https://github.com/SwordInfra/SwordFS/actions/runs/40429424432)
executed the Slice experiment against fresh Redis/MinIO/FUSE volumes:

- `pjdfstest` strict classification **8,770/8,770 PASS**, 28
  upstream-not-applicable, **0 blocking**.
- Four actual Slice `fstests` cases `generic/001`, `generic/002`,
  `generic/004`, `generic/005`: all **PASS**, `check.exit=0`, no skipped or
  failed XUnit cases; verified the raw artifact `slice-posix-experiment`.
- The first Slice fstests classifier failed **after** successful test execution
  because its `--known-gap-evidence-dir` pointed at COW's historical
  `conformance/fstests/fail-evidence`. With zero Slice known gaps, these COW
  evidence files appeared as orphaned entries. The CI Slice classifier now
  creates and uses an **empty Slice-only evidence directory**; the same strict
  classifier rerun on the actual CI raw files passes **4/4**, zero blockers.
  A focused tooling regression explicitly guards against reusing COW's
  historical fail evidence when Slice has no admitted gaps.
- Separate `chunk_type=chunk_slice` evidence is required on both suites;
  neither result changes the existing COW support lists or published reports.

A subsequent exact-final-SHA formal CI run must pass before #510 is accepted.
This first Slice fstests smoke does not establish full fstests parity.
