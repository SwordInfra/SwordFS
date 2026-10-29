# #525 — Diagnose Redis retry exhaustion during Slice aio-stress

## Failure contract

`generic/113` is pinned upstream `aio-stress`: it exercises single-file and
multi-process asynchronous writes, including direct I/O variants. The
upstream checker can report PASS even when a later FUSE Flush fails. The
SwordFS strict TEST/SCRATCH writeback audit is therefore an independent,
mandatory correctness gate, not a historical compatibility test.

Two separate executions reported the same failure:

- PR #520 CI `40649911892` (2026-10-28): `generic/113` XUnit PASS, TEST
  daemon `FileReadWriter::Flush chunk FAILED` after `Redis transaction retry
  limit exceeded`; tracked in #525, provisionally believed mitigated by #524.
- Post-#528 main CI `40804825940` (2026-10-29, commit `87b8781`):
  `generic/113` XUnit PASS (32 seconds), TEST daemon inode 3852/chunk 0
  emitted the same Redis retry-exhaustion failure. All ten fstests shard
  jobs passed their upstream harnesses; the strict aggregate gate correctly
  classified `generic/113` as `WRITEBACK_FAILURE` and failed. Redis was
  healthy, with no observed container restart or OOM.

The 2026-10-28 closure of #525 was based on several passing runs, **not** a
deterministic root-cause demonstration. The recurrence invalidates treating
those passing runs as proof that concurrent publication is fully safe.

## Transaction boundaries and attribution

`RedisMetaClient::Transact` retries only operations with a known safe
pre-commit boundary: `RedisKvTxn::WatchConflict`, a transport failure while
WATCH/reading/queuing before EXEC, or a failure acquiring a connection before
any transaction was started. It must **never** blind-retry unknown outcomes
after issuing EXEC; `RedisKvTxn::Commit` returns `OutcomeUnknown` for those.
An exhausted budget currently returns the same `Unavailable` message
regardless of which retryable boundary failed, making a single TEST daemon
writeback line insufficient to identify the mechanism responsible.

Preserve the bounded retry count and backoff while instrumenting exhaustion
with mutually exclusive counts for (a) WATCH conflicts, (b) pre-EXEC
transport/protocol errors, and (c) connection/acquisition failures. The
counts must sum to the configured retry budget when it is exhausted. The
returned `Status` remains `Unavailable`, so callers still report a failed
Flush and strict CI remains red. No credentials, Redis key contents, or
opaque request payloads should be logged.

Deterministic unit tests first validate attribution for all-WATCH conflict
and all-connection-failure retries. Existing tests continue to prove that
post-EXEC ambiguous results are terminal without replay.

## Investigation and fix acceptance

1. Establish RED/GREEN for the diagnostic contract using focused Debug UT.
2. Run pinned `generic/113` with the strict writeback audit enabled and
   preserve both TEST and SCRATCH daemon logs. Distinguish observed WATCH
   contention from pre-EXEC unavailability using the new counters. A green
   replay is only evidence of intermittency, not proof of a production fix.
3. If WATCH contention is confirmed, identify the actual competing inode
   mutation path, including cross-handle metadata calls not covered by the
   per-inode Slice publication lock. Fix ownership/serialization or reduce
   unnecessary WATCH conflicts without losing Redis authoritative CAS.
   If pre-EXEC transport is confirmed, address connection/resource health
   rather than extending the retry window indiscriminately.
4. Add an executable regression for the established root cause. Verify the
   corrected production behavior via focused UT and required main-equivalent
   full CI, including the strict `generic/113` supported shard. Keep the
   failed original CI artifact as evidence. Do not add a known-gap waiver,
   remove writeback auditing, inflate retry budgets without attribution, or
   interpret a single passing run as a guarantee of reliability.

Stage A #528 was tests/docs-only and did not change Redis retry semantics;
its merge was the trigger for the observed main CI, not proof of causation.
