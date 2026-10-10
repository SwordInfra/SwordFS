# Redis independent-name concurrency contract diagnostics (#454)

## Contract and failure evidence

Two independent `RedisMetaImpl` instances sharing a formatted Redis volume
must both create their 20 **distinct** names and leave all 40 names reachable.
A failed request is a contract failure, not an expected race or retry hint.

The Debug unit suite on PR #452 (run 38021490365) and a focused single-test
reproduction (run 38022100856) both observed one missing `second-*` entry.
The old test only counted failures; it discarded the actual Create status.
Its fatal Lookup assertion also bypassed the thread-domain Redis-peer reset,
producing a *second* execution-domain fatal error in its cleanup path.

## Diagnostic correction

- Record per-name Create results without concurrent writes to shared counters
  or unsynchronized failure-message containers; each writer owns one status
  array and the test examines it only after joining the threads.
- Report both operation error codes/messages and lookup outcomes for each
  name, without changing success expectations or adding arbitrary sleeps.
- Guarantee Redis peer destruction in the correct POSIX-thread domain even
  after a failed/fatal GTest assertion via a local scope guard.
- Run the exact test in dedicated Debug CI to identify the root status before
  determining if any Redis implementation change is required. Keep #440's
  unrelated upper-layer fixture migration out of this PR.

## Root cause and bounded remediation

Focused Debug CI 38022520657 showed `second-0` failed with
`Redis transaction retry limit exceeded` and its name was absent. In a
same-parent concurrent Create, both Redis engines WATCH the parent inode;
the default three transaction attempts can repeatedly collide while the
other writer remains active. This is a retry-liveness defect under ordinary
metadata contention, not a Redis transport failure or a cleanup-only issue.

The production default now allows eight bounded attempts with the existing
randomized exponential backoff; an explicit configuration value still controls
the limit. No extra caller-level retry is added, and post-EXEC uncertain
outcomes remain non-retryable. The two-client, 40-distinct-name contract stays
strict and reports per-name failures; all other retry-exhaustion tests retain
their explicit small budgets. The canonical client policy is recorded in
`docs/design/redis-metadata-schema.md`.
