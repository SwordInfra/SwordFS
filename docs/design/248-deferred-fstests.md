# Issue #248: Bounded execution for deferred fstests

## Goal

Return `generic/069`, `generic/471`, and `generic/478` to real SwordFS execution without allowing one slow or blocked FUSE testcase to make the normal PR conformance gate unbounded.

The upstream testcases remain unchanged. SwordFS changes only its CI execution and classification surface.

## Current problem

The three cases are selected by pinned `generic/quick`, but `deferred-ci.tsv` removes them from the deterministic shard plan. The existing runner isolates testcases, but its `timeout` uses the remaining **whole-shard** budget. A blocked testcase therefore terminates the shard and prevents later cases from producing evidence.

The cases have different risk profiles:

- `generic/069` runs six concurrent O_APPEND writers, including a 3,000,000-integer workload. It is expected to be slow on FUSE but is otherwise finite.
- `generic/471` validates POSIX `rewinddir(3)` visibility using the upstream `rewinddir-test` helper. Its behavior should be characterized directly rather than inherited from another FUSE implementation.
- `generic/478` exercises many OFD/POSIX lock combinations coordinated by SysV semaphores. If one lock operation never completes, the upstream testcase can wait indefinitely.

## Design

### SwordFS-specific characterization evidence

Draft CI run `35998995156` executed each testcase independently against the real Redis + MinIO + FUSE path:

- `generic/069`: PASS in **180s**;
- `generic/471`: PASS in **47s**;
- `generic/478`: bounded FAIL in **459s**, with repeated `setlkw: Function not implemented` output. SwordFS `SwordFsGetlk`/`SwordFsSetlk` return `ENOSYS`, so this is the existing file-lock capability gap tracked by #255, not an infrastructure hang.

### Authoritative bounded shards

The final design removes all three entries from `deferred-ci.tsv`. `generic/069` and `generic/471` enter `supported.txt`; `generic/478` enters `known-gaps.tsv` as `known_unsupported` / #255 with its exact normalized output-mismatch signature.

`conformance/fstests/bounded-ci.tsv` maps each testcase to a dedicated authoritative shard. `plan.py` removes bounded cases from the normal supported/rest balancing and emits those shards as part of the same deterministic plan consumed by `aggregate.py`. The CI matrix executes each bounded shard with a **15 minute suite budget** and **35 minute job budget**. Their artifacts use the normal `fstests-conformance-*` prefix, so aggregate/classifier requires and classifies them exactly like every other shard.

This preserves isolation: a future 478 hang becomes a blocking failure in its own bounded shard and cannot hide 069, 471, or any existing baseline testcase.

`run.sh` accepts an explicit `--suite-timeout` override so bounded shards can use a tighter watchdog without duplicating the runner.

## Invariants

- Upstream testcase sources are never edited to make SwordFS pass.
- A slow/hung testcase cannot consume another testcase's evidence budget.
- Experimental characterization results cannot silently affect the supported/known-gap aggregate.
- Once admitted, every testcase is part of the normal auditable classifier population.
- A timeout is not automatically an infrastructure error when the filesystem behavior itself prevents a bounded testcase from completing; root cause must be established before classification.

## Verification

Local verification is limited to planning/script/YAML checks. Real fstests/FUSE execution is GitHub-CI-only.

Before Ready/merge:

- each of 069/471/478 has SwordFS-specific CI evidence;
- each is removed from `deferred-ci.tsv`;
- each executes in a bounded authoritative surface;
- aggregate/classifier remains strict and complete;
- all normal CI/conformance gates pass.
