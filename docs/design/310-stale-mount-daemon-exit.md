# Issue #310: Stale-mount daemon exit synchronization

## Problem

The stale-mount E2E tests checked daemon shutdown by scanning `/proc` for every
process whose command line contained `swordfs` immediately after unmount.
Unmount completion and daemon process exit are distinct events, so this global
instantaneous count races normal daemon shutdown and can also observe unrelated
SwordFS processes. Coverage-instrumented E2E execution made the race visible.

## Design

Use the existing fixture-owned daemon PID as the synchronization boundary.
`Fixture::IsDaemonGone()` waits for that exact process to disappear for a bounded
period after unmount. This is the same lifecycle assertion already used by the
regular mount E2E tests.

Remove the global `/proc` scan entirely. The tests continue to require that the
daemon created by their own fixture exits; they simply wait on the correct
identity instead of assuming all SwordFS processes disappear synchronously.

## Non-goals

- no arbitrary sleep to mask the race;
- no production daemon/test mode changes;
- no global process killing or cleanup;
- no weakening of the daemon-exit semantic.

## Verification

The E2E target must compile locally. Actual mount/unmount execution remains
GitHub-CI-only. The originating failure was recorded on PR #308 run
`35977094178`, E2E job `107561240731`.
