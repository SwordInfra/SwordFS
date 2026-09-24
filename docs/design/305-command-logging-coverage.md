# Issue #305: Command and Logging Coverage

## Goal

Raise `src/cmd/Main.cpp`, `src/cmd/Format.cpp`, `src/cmd/Mount.cpp`, and
`src/utils/Logging.cpp` to the #302 **>=80% production-source coverage floor**
without exposing process helpers or adding production code that exists only for
tests.

## Baseline and measurement finding

The #301 E2E coverage session shows that the existing real CLI/FUSE workload
already executes these files, but its first two-session union still reports:

- `Main.cpp`: 55.55% on main after #301 exposed additional real-process lines;
- `Format.cpp`: 60.00%;
- `Mount.cpp`: 41.32%;
- `Logging.cpp`: 60.00%.

The low `Mount.cpp` result is partly a measurement effect of daemon mode:
`Daemonize()` uses a double fork and `_exit()` in the parent/intermediate
processes. Those process-local gcov counters are not a reliable way to measure
the complete mount lifecycle.

## Design

Keep all new coverage at public process/API boundaries.

### Main

Exercise the real process entry point for three contracts that remain uncovered
after the initial command E2E slice:

- no subcommand prints help and exits successfully;
- an invalid option is translated through CLI11's parse-error path;
- `SWORDFS_ENABLE_COREDUMP` activates the process-level coredump setup branch.

These are process contracts and are tested by spawning the real `swordfs`
binary; no process-setup helper is exported for tests.

### Format

Execute the real `swordfs format` binary twice against one unique Redis/S3
volume. The first invocation establishes a valid volume; the second must fail
through `RunFormat()`'s production error path. This validates error
translation rather than directly invoking internal metadata helpers.

### Mount

Use the real `swordfs mount` binary for public lifecycle/error contracts, including:

- reject `/` as a mountpoint;
- reject an existing regular file as a mountpoint;
- report a missing volume in foreground mode;
- complete a real foreground mount lifecycle: formatted Redis/S3 volume,
  FUSE-ready mount, rejection of a second mount on the same mountpoint,
  external unmount, and normal process exit;
- reject a dash-prefixed mountpoint passed as a positional argument;
- reject a mountpoint whose parent cannot be created;
- create a missing mountpoint before reporting a missing-volume failure;
- report a real libfuse mount failure on an existing mountpoint not owned by the caller;
- reject a stale FUSE mount after the daemon is killed, then force-unmount it during test cleanup;
- exercise daemon-mode success with a real pidfile and bounded daemon exit;
- exercise daemon-mode missing-volume failure so the parent/pipe failure
  propagation path is validated without syscall interposition.

Foreground mode is deliberate: it exercises the same production
`RunMount() -> Mount() -> libfuse` path while allowing one process to return
normally and flush its coverage counters. It is not a test-only production
mode.

The coverage audit also found a redundant `RunMount()` defensive branch that
re-read `SelectedSubCommand()` only to test for null and never used the returned
command. `main()` already performs this dispatch guard before calling `RunMount`.
The redundant branch is removed instead of manufacturing a direct-call test for
an impossible CLI state.

### Logging

Validate the public `InitLogging()` contract with subprocess/death tests for:

- an invalid log level;
- an empty background log path;
- an unopenable background log path.

The real foreground/background success paths remain exercised by command E2E.
No private logging helper is exported.

## Debug service-backed coverage

Issue #301 established that Release-optimized coverage can under-report source
lines that real E2E tests execute. The stacked #305 change therefore extends
the same instrumented Debug service-backed step to run
`CommandCoverageE2ETest.*` alongside the focused S3 contract before the normal
Debug lcov capture.

This does not duplicate the full E2E suite and does not change production
behavior. It executes the real `swordfs` binary, Redis, MinIO, and foreground
FUSE lifecycle from an unoptimized instrumented build so Format/Mount coverage
reflects the tested process contracts accurately.

## Non-goals

- no public exposure of `ValidateMountpoint`, `Daemonize`, or `Mount`;
- no syscall interposition solely to force `pipe()`, `fork()`, or libfuse
  failure branches;
- no coverage exception merely because code is CLI/process-oriented;
- no weakening of daemon semantics for gcov.

If exact fatal/OS-failure-only branches remain below the floor after the
behavioral tests, any exception must satisfy the full file-specific #302
evidence contract.

## Verification

Deterministic Logging death tests may run locally. Command/FUSE tests are
service-/privilege-dependent and execute only in GitHub CI through the #301 E2E
coverage path. Final readiness requires per-file Codecov reconciliation against
the current main branch.
