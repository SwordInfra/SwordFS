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

The stale-daemon assertion also enumerates only actual `/proc` process entries.
Directly probing arbitrary `/proc/<id>/cmdline` paths is incorrect on Linux because
non-leader thread IDs are addressable there too and would make one multithreaded daemon
appear as several processes.

### Failure diagnostics lifecycle

E2E daemon diagnostics have a longer lifetime than the disposable mount work directory.
Each test owns a directory under `SWORDFS_E2E_DIAGNOSTICS_DIR`; format output goes to
`format.log`, while each daemon generation writes to a distinct `mount-<generation>.log`.
This separation is required because persistence and recovery tests can remount several
times before their final assertion. Internal remount cleanup removes mount/config/workdir
state but deliberately retains every daemon-generation log, so a later disconnect cannot
erase the log from the daemon that caused it.

`Fixture::TearDown()` is not necessarily the final GTest teardown: mount-lifecycle tests
also call it explicitly inside one test body before mounting again. The fixture therefore
never deletes its diagnostics directory. It takes a non-blocking liveness snapshot before
cleanup and turns an already-missing daemon or an incomplete mount shutdown into a GTest
failure, ensuring an unexpected daemon disappearance cannot look like a successful test.
Likewise, `Remount()` is the clean-restart contract and rejects a daemon that has already
disappeared before cleanup; only `CrashAndRemount()` intentionally accepts a killed daemon.

The suite wrapper owns invocation-level diagnostics cleanup instead. `run-e2e.sh` exports
one diagnostics root, clears any stale evidence before starting dependencies, and removes
the root after a successful E2E binary unless `SWORDFS_E2E_KEEP_WORKDIR` explicitly asks
to retain test state. A failed assertion, client-visible FUSE disconnect, teardown
failure, or daemon disappearance makes the binary non-zero and preserves all
per-test/generation logs. The GitHub E2E job uploads that diagnostics root only on
failure, so successful CI runs retain neither per-test diagnostics nor a persistent
artifact.

CI also runs one disabled diagnostics-contract case explicitly before the normal suite.
That case performs a remount and then deliberately fails; its wrapper expects the non-zero
result and verifies that both daemon-generation log files survived final teardown before
removing the controlled evidence. The wrapper also deletes `.gcda` counters produced by
this intentionally failing smoke case before the normal suite, so diagnostics verification
does not inflate the E2E Codecov session. This provides executable proof of the
failure-retention and multi-generation contracts without making the required E2E suite
intentionally red or contaminating coverage evidence.

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
  translation, Delete, idempotent repeated Delete, and propagation of Put/Delete
  failures from a deliberately absent bucket. Runtime operations execute from a
  real Folly fiber, matching the production execution-domain contract.

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

## Debug service-backed coverage refinement

The two-session report exposed a coverage-fidelity limitation in the Release
E2E trace: the MinIO contract test executes the S3 Put/Get/Delete paths, but
optimization causes many executed source lines in `S3DataEngine.cpp` to remain
reported as missed/partial. Adding more assertions to the same Release binary
would therefore optimize for instrumentation artifacts rather than behavior.

Keep the full Release E2E coverage upload because it provides useful real-process
coverage for command/FUSE paths. In addition, the existing instrumented Debug
coverage job builds `swordfs_e2e_test` and runs only
`S3DataEngineE2ETest.*` through `run-e2e.sh` before the normal Debug `lcov`
capture. This reuses the real Redis + MinIO dependency lifecycle but does not
duplicate the full E2E suite. The focused service-backed contract therefore
contributes accurate Debug line coverage to the same authoritative unit/Debug
trace.

This remains a real production boundary: no AWS client is mocked, no S3 private
helper is exposed, and the test assertions are unchanged.

## Non-goals

- no fake AWS client or test-only storage API;
- no public exposure of `Mount.cpp` process helpers merely for unit testing;
- no duplicate E2E job solely for coverage;
- no weakening of the existing Release E2E workload.

## Verification

GitHub CI is authoritative because the coverage source is the service-backed,
FUSE-enabled E2E environment. The final PR must preserve all existing required
checks and Codecov must be inspected per target production file.
