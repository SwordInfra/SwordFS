# Test contract audit (#355)

## Purpose

SwordFS tests should protect stable filesystem, storage, and concurrency semantics without turning incidental implementation choices into permanent contracts. This audit classifies literal values, timing, and exact mechanism counts by the semantic property they prove, then removes coupling only where a valid implementation change could preserve behavior but still fail the test.

This is a test-quality/refactor task. It does not change production behavior and does not introduce production-only test seams.

## Classification model

Candidates are reviewed as one of four categories:

1. **Stable contract** — POSIX/protocol/persistence semantics and intentionally public defaults remain exact.
2. **Scenario data** — values chosen to construct a boundary, fault, or workload remain local to the scenario; they may be named when that clarifies intent.
3. **Intentional mechanism invariant** — exact retries/calls remain only when they prove ownership, serialization, publication ordering, replay safety, or another mechanism that is itself part of correctness.
4. **Accidental implementation coupling** — current policy/tuning values or scheduler timing are replaced by observable-state, relationship, or deterministic coordination assertions.

## Baseline inventory

The repo-wide discovery pass covered `tests/` and `conformance/` using timing literals/sleeps, retry/config policy names, and exact count/call assertions as discovery patterns. The patterns are intentionally over-inclusive; each hit is classified by its test intent rather than changed mechanically.

The baseline contained:

- 40 timing/config-duration candidate lines across E2E, Redis, utility, FUSE, and reclaimer tests;
- 166 exact count/call/retry candidate lines across 18 test files;
- explicit policy/default assertions for Redis client defaults, mount thread-count parsing, FUSE cache timeout values, and storage worker option propagation.

### Timing and policy candidates

| Area | Classification | Decision |
| --- | --- | --- |
| `BlockingExecutorTest` fixed `10ms`/`50ms` sleeps | Accidental coupling | Replace scheduler timing with rendezvous/release events plus generous liveness watchdogs so regressions fail instead of hanging. |
| `FiberRuntimeTest` fixed sleeps used to keep work pending | Accidental coupling | Replace with start/release batons; bound admission-transition polling and shutdown completion with named liveness watchdogs. |
| FUSE `attr_timeout` / `entry_timeout == 1.0` assertions | Accidental policy coupling | Remove the numeric assertions from tests whose contract is authoritative attributes / mknod behavior. The production cache policy remains unchanged. |
| E2E mount/process polling | Test watchdog / external-state polling | Keep bounded polling: the observable condition is an external process or kernel mount state and no SwordFS-internal event seam should be added for tests. |
| Reclaimer worker polling | Test watchdog / observable durable state | Keep bounded polling: tests watch durable reclaim state instead of exposing worker internals. |
| `S3DataEngineTest` slow-response `50ms` delay | Scenario data | Keep: the delay constructs an intentionally slow but progressing HTTP response to exercise an independent request deadline. |
| Redis retry `0ms` backoff and short socket timeout | Scenario/fault data | Keep: these values eliminate irrelevant waiting or deterministically induce an ambiguous timeout. |
| `RedisMetaConfigTest` default durations/retry count | Stable config contract under test | Keep exact: the test is explicitly the parser-default contract. |
| Redis explicitly supplied client-option values | Scenario round-trip | Keep exact: the values prove that user-provided configuration is parsed without substitution. |

### Exact mechanism/count candidates

| Area | Classification | Decision |
| --- | --- | --- |
| Redis transaction `attempts` counts | Intentional mechanism invariant | Keep. Exact `1/2/N` attempts prove retry ownership, WATCH-conflict replay, retry-budget exhaustion, and no replay after ambiguous EXEC. Test names make the reason locally visible. |
| `FileReadWriterTest` put/revision/find/replace counts | Intentional mechanism invariant | Keep. These tests encode the chunk publication state machine: fresh revisions, no replay of ambiguous writes, COW generation ordering, and retry reconciliation. |
| Mem metadata concurrency success/failure counts | Stable concurrency contract | Keep. Exact counts prove atomicity/TOCTOU behavior, not an implementation call sequence. |
| File/VFS zero-call assertions (`lookup`, per-entry inode fetch, reclaim on close) | Intentional mechanism invariant | Keep where test names/comments state the required ownership or avoided extra metadata operation. |
| ConfigCenter / VolumeImpl worker counts | Scenario round-trip | Keep exact because the value was supplied by the test and the contract is propagation, not the particular number. |
| Chunk/file sizes, inode ids, mode bits, errno values | Stable contract or scenario data | Keep; do not abstract merely because the values are numeric. |
| READDIRPLUS `128` attribute batch cap and tests built around exactly 128 entries | Accidental tuning coupling | Replace with reply-buffer behavior tests independent of the internal batch width and a relationship-based assertion that a large directory is split into bounded metadata batches. |
| Reclaimer `128` pending-delete batch position | Accidental tuning coupling | Use a large backlog and assert the orphan is serviced between bounded passes rather than at an exact numeric index. |
| Dir iterator seek counts and publication/concurrency call counts with explicit ordering names | Intentional mechanism invariant | Keep because serialization/order is the behavior being tested. |

## Resulting invariants

After the audit, the affected tests express these stable properties:

- blocking executor fibers make progress because blocking work is handed to independent workers, proven with explicit coordination rather than wall-clock sleeps;
- fiber runtime shutdown stops admission and drains already admitted tasks before returning;
- FUSE attribute correctness tests do not freeze the current cache timeout policy;
- READDIRPLUS stops correctly at reply-buffer boundaries regardless of the current metadata batch width;
- READDIRPLUS still performs bounded metadata batching for sufficiently large directories without declaring a particular tuning value as API;
- the reclaimer interleaves orphan cleanup with a large pending-delete backlog and self-wakes to continue work, without copying the current pending-delete batch size into the test;
- exact retry/publication/concurrency counts remain where changing the count would change correctness or ownership semantics.

## Deliberately retained timing

Not every deadline is brittle synchronization. The audit keeps bounded waits that are test watchdogs around external or asynchronous observable state, including FUSE reply capture, mount/process lifecycle, Redis timeout fault injection, and reclaimer durable-state progress. Removing those deadlines would weaken liveness checks or require test-only production hooks.

## Verification plan

This change touches test code and this design document only, so C++ production patch-coverage gating is not applicable.

Verification requires:

1. build the affected unit-test targets with local compile parallelism limited to half of logical CPUs;
2. run the focused `BlockingExecutor`, `FiberRuntime`, FUSE/VFS, and reclaimer unit tests locally;
3. run repository formatting/pre-commit and `git diff --check` after all files are tracked;
4. use GitHub CI as the authoritative full-suite verification, including service-backed and E2E coverage;
5. perform a final readability/test-quality review to ensure no retained exact count lacks a semantic reason and no production test seam was introduced.

## Non-goals

- changing Redis/FUSE/reclaimer production policy values;
- eliminating exact values that are protocol/POSIX contracts or explicit user-input round trips;
- weakening retry, publication, serialization, or atomicity tests simply to reduce mock call counts;
- introducing generic test DSLs or production hooks to make tests observe internals.
