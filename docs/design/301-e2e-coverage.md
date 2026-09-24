# Issue #301: Service-backed E2E Coverage

## Problem

SwordFS already exercises Redis, MinIO/S3, the real CLI, daemonized mount, FUSE,
and storage IO in `e2e-test (Release)`. Codecov, however, only receives a trace
from the Debug unit-test job. Production paths that are deliberately tested
through real services therefore appear uncovered even though the E2E suite
executes them.

This measurement topology is the main reason the #302 source-coverage floor
shows very low coverage for `S3DataEngine.cpp`, `S3StreamBuf.cpp`, `Format.cpp`,
`Mount.cpp`, and `Logging.cpp`.

## Design

Keep the existing Release E2E job and its required-check identity. Add GCC
coverage instrumentation to that Release build, capture the resulting trace
after the existing E2E suite completes, remove system/third-party/build paths,
and upload the trace as a second Codecov session for the same commit.

This does not add a second E2E execution and does not replace the Debug unit
coverage session. Codecov combines the independently collected execution
signals for production sources.

Coverage instrumentation also makes daemon process teardown slightly slower because the
process flushes runtime counters during normal exit. The E2E fixture therefore treats
filesystem unmount and daemon termination as distinct lifecycle events: after a
successful `fusermount3 -u`, it waits on the recorded daemon PID with a bounded poll
before declaring teardown complete. The last daemon PID remains recorded after exit so
existing post-teardown assertions can verify that exact process is gone. This is a
synchronization invariant, not a sleep used to mask failure; a daemon that remains alive
after the bound still fails teardown.

The E2E fixture already supplies the meaningful contracts behind the trace:

- `Fixture::FormatVolume()` shells out to the real `swordfs format` command;
- `Fixture::StartMount()` shells out to the real daemonized `swordfs mount`;
- `scripts/testing/run-e2e.sh` provisions Redis and MinIO and creates the S3
  bucket used by the mounted filesystem;
- the E2E suite performs real filesystem operations through FUSE and validates
  persistence/recovery behavior.

Therefore the additional coverage is evidence from existing behavioral tests,
not synthetic line execution.

## Coverage collection

The Release preset remains active. `--coverage` is added to compile/link flags
for this job only. After E2E succeeds, `lcov` captures `build/`, applies the same
branch filters used by the Debug unit job, removes system, fetched dependency,
and build-generated paths, and uploads `e2e-coverage.info` to Codecov with the
`e2e` flag.

A non-empty filtered trace is required before upload. The Codecov action keeps
the repository's existing external-service failure policy, while #301 itself
is not Ready until the resulting Codecov report is inspected and the target
storage files satisfy the #302 floor or receive an explicit justified
exception.

## Residual coverage remediation

The first authoritative two-session report on rebased head `e63baba` proves the
measurement topology works, but it also shows that measurement alone is not the
completion criterion. The union reports `S3DataEngine.cpp` at **62.01%**,
`S3StreamBuf.cpp` at **77.77%**, and `StorageUrl.cpp` at **89.47%**. Therefore
#301 adds only the remaining meaningful storage contracts instead of treating
trace upload as success.

The residual tests use two legitimate boundaries:

- deterministic unit tests validate invalid bucket locations, object-key
  identity without a configured prefix, and the preallocated response-stream
  contract (bounded writes, current position, flush, unsupported seek, and
  overflow);
- a MinIO-backed E2E test exercises `S3DataEngine` directly through its public
  `IDataEngine` contract: prefixed object identity, complete Put/Get, bounded and
  remainder range reads, undersized caller-buffer rejection, missing-object
  translation, Delete, and idempotent repeated Delete. Runtime operations execute from a real Folly fiber, matching
  the production execution-domain contract.

No AWS client is mocked and no private helper or production test hook is
exposed. Service-backed execution remains GitHub-CI-only.

## Defect discovered by residual coverage

The MinIO-backed bounded-read contract test produced a genuine RED on PR #308
(run `35977094178`): requesting four bytes into a caller buffer with only three
bytes of tailroom returned `EIO` instead of `EINVAL`. The existing capacity
check ran only after `GetObject()` succeeded, but the AWS SDK failed first while
flushing the response stream into the undersized buffer, making the intended
validation unreachable for this case.

The production fix rejects a bounded request when `size > out->tailroom()`
before issuing S3 IO. This preserves the `IDataEngine::Get` caller-capacity
contract, returns the correct argument error, and avoids an unnecessary network
request. The same precondition is covered by a deterministic unit test so the
Debug per-file patch-coverage gate verifies the production fix independently of
the service-backed E2E session. The post-response content-length check remains necessary for
zero-length/remainder requests whose response size is not known before IO.

## Non-goals

- no fake AWS client or test-only storage API;
- no public exposure of `Mount.cpp` process helpers merely for unit testing;
- no duplicate E2E job solely for coverage;
- no weakening of the existing Release E2E workload.

## Verification

GitHub CI is authoritative because the coverage source is the service-backed,
FUSE-enabled E2E environment. The final PR must preserve all existing required
checks and Codecov must be inspected per target production file.
