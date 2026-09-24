# Issue #310: Synchronize stale-mount daemon exit by fixture identity

## Problem

The stale-mount E2E tests historically checked daemon shutdown by scanning every
process under `/proc` and treating any command line containing the substring
`swordfs` as a SwordFS daemon. That is not a process-identity contract: unrelated
processes can legitimately carry `swordfs` in an argument or temporary path.

The flaw was reproduced again while #321 moved MinIO from a container to a
runner-owned process. MinIO's data argument used `/tmp/swordfs-minio-test-*`, so
`CountSwordfsDaemons()` counted MinIO as a SwordFS daemon even though the tested
mount daemon had already exited. PR #322 E2E run `36009201277` therefore passed
149/151 tests and failed only the two stale-mount global-count assertions with a
count of one.

## Design

A `Fixture` owns the exact PID of the SwordFS daemon it started. The daemon-exit
contract is therefore expressed through `Fixture::IsDaemonGone()`, which performs
a bounded wait on that PID. `Fixture::StopMount()` already uses the same helper
before teardown returns.

The stale-mount tests now assert `fixture_.IsDaemonGone()` after teardown and
remove `CountSwordfsDaemons()` entirely. This eliminates global `/proc` scanning,
substring process identification, unrelated-process interference, and duplicated
shutdown synchronization logic.

No production hook, sleep, process-killing mode, or production behavior changes.

## Verification

- compile `swordfs_e2e_test` locally; real FUSE execution remains GitHub-CI-only;
- `StaleMountTest.DaemonExitsAfterUmount` and
  `StaleMountTest.DaemonExitsAfterMultiCycle` must pass in GitHub E2E;
- full required CI must remain green.
