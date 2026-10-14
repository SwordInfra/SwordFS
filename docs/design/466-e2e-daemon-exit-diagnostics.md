# E2E daemon exit diagnostics (#466)

## Failure and evidence

The Release E2E job in PR #465, run `38378992797` attempt 1, lost its
FUSE connection during `PersistenceTest.CrossChunkTruncatePersistsPrunedMetadataAcrossRemount`.
`fsync` returned `ECONNABORTED`, while the corresponding mount log stopped
after normal initialization. The identical SHA passed a targeted job rerun.
Neither a COW/shutdown defect nor runner interference has been established.

## Diagnostic contract

The E2E fixture owns the mount process as a **direct child**. It uses the
existing `mount -f` interface to avoid double-forking, which previously made
`waitpid` impossible. Every mount generation has its own log and, once the
child exits, a durable `mount-N.exit` record containing its PID and exact
exit status (normal exit code, terminating signal, or other wait status),
and whether the fixture requested clean unmount, intentional crash, or
forced cleanup before that exit.
The fixture reaps each child exactly once and preserves exit reports even
when the test's scratch mount directory is removed. Expected SIGKILL in a
crash/remount test remains recorded but is not misclassified as an unexpected
test failure. Unexpected disappearance before teardown remains a test failure.

On Release E2E failures, CI collects bounded kernel, core-dump inventory,
and cgroup OOM/memory evidence into the **same diagnostics artifact**.
Collection is best-effort and must never replace the original failure
status. The controlled failing diagnostic test verifies that clean unmount
and intentional SIGKILL each produce the expected generation report.

## Verification and limits

- The required E2E CI job checks that the controlled failure retains logs
  from successive mount generations and precise exit statuses.
- Normal Release E2E/UT/conformance gates and Codecov remain unchanged.
- For a new unexplained disconnect, first inspect `mount-N.exit`, daemon
  stderr, kernel/cgroup/core evidence, and CI runner context before changing
  storage or FUSE invariants. A test or production race fix requires separate
  reproduction evidence.
- The original run has no exit-status/core evidence and cannot be
  retroactively classified; `SIGKILL` alone does not prove OOM without
  corroborating cgroup/kernel records.

## Root-cause investigation (in progress)

This document is not a closure claim: the original objective is to explain
and, if confirmed, repair the intermittent FUSE disconnect.

The original job logs show an `ECONNABORTED` from the sparse two-chunk
`fsync`, immediately followed by an unmount reporting `EBUSY` and
`Transport endpoint is not connected` errors during cleanup of later test
mountpoints. Those errors alone do not establish which event occurred first
inside the daemon. The earlier full-chunk persistence test in that same job
passed, but it does not exercise the identical sparse-chunk flush shape.

The manually dispatched `Dev Build` E2E mode runs an exact Git SHA with the
same Release `--coverage` instrumentation as the failing job. Each of up to
20 repetitions starts fresh Redis/MinIO backends and executes only a bounded
GTest selection. Compare the original failing `d91d152` revision against
current `main`, using the same target filter. This experiment is intentionally
not a required check, does not weaken any existing CI gate, and is separate
from the complete PR E2E suite. Inspect signal/core/cgroup evidence if it
fails; a finite clean run cannot exclude a rare failure.

If a failure reproduces, inspect the exact terminating condition and the
related COW metadata/flush/shutdown path before proposing a production fix.
Only a demonstrated violated invariant justifies a test-first production
change. A non-reproduction alone is not proof of infrastructure flakiness.
