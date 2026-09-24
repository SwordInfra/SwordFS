# Production Test Coverage

## Purpose

SwordFS measures repository coverage against production sources under `src/`.
Test sources are excluded from the Codecov project total because executing a
test file is not evidence that production behavior is validated correctly.

The coverage program uses uncovered production paths as a discovery signal.
Its primary goal is to strengthen executable contracts around filesystem,
metadata, storage, recovery, and lifecycle behavior. Increasing the percentage
is an expected result of better tests, not a reason to encode the current
implementation as the expected behavior.

## Test construction contract

Before adding a test for an uncovered path, establish the expected behavior
from the strongest available contract: POSIX/filesystem semantics, a SwordFS
design invariant, a public interface contract, or an explicitly documented
project decision. The assertion should describe that behavior rather than copy
the implementation's current result.

If a new behavior-validating test fails because production code violates the
contract, the failure is a production defect to investigate. The test must not
be weakened merely to turn the build green. A substantial production defect
discovered during coverage work should be tracked and fixed with the normal
bug-fix workflow, including test-first RED/GREEN evidence when the fix changes
C++ production behavior.

Coverage work must not introduce production-only-for-test setters, hooks,
flags, friend access, or branches. If correct scenario construction is
unnecessarily difficult, prefer a production refactor that improves the real
abstraction boundary rather than exposing a test seam with no production use.

## What counts as useful coverage

Useful coverage validates an observable contract or a meaningful invariant,
including success/error translation, state transitions, lifetime ownership,
failure propagation, recovery behavior, boundary conditions, and concurrency
semantics. Merely executing a line, testing trivial accessors for percentage
gain, or mocking away the behavior under test does not qualify.

Tests should use the narrowest production boundary that still exercises the
contract. Unit tests are preferred for deterministic local semantics. Existing
service-backed or FUSE integration paths remain appropriate when the contract
cannot be represented faithfully by an in-process unit boundary; those paths
are verified in GitHub CI rather than through ad-hoc local services.

## Baseline and prioritization

At the start of Issue #298, `main` at `803b2b4` reports **76.75%** production
line coverage (4723 hits over 6153 executable lines). The largest high-value
gaps include:

- `src/fuse/Vfs.cpp`: 31.74%, with most callback forwarding/reply paths
  uncovered despite those paths defining the kernel/userspace boundary;
- `src/metadata/redis/RedisMetaTxn.cpp`: 67.50%, with uncovered validation,
  failure, and transaction-state branches in authoritative metadata logic;
- `src/storage/s3/S3DataEngine.cpp`: 37.20%, with storage request/error paths
  that require careful separation between deterministic unit behavior and
  service-backed verification;
- `src/metadata/mem/MemMetaImpl.cpp`: 78.15%, with remaining metadata error and
  boundary paths useful for checking parity with Redis-backed semantics.

Prioritization is based on semantic risk and the probability of finding real
production defects, not on the cheapest way to increase the aggregate number.
CLI-only and trivial utility paths therefore rank behind the filesystem,
metadata, and storage boundaries even when they have lower percentages.

## Execution strategy

Coverage remediation proceeds in bounded slices. For each slice:

1. identify uncovered production branches and their intended contract;
2. add assertions at a legitimate production boundary;
3. investigate surprising failures as possible production defects;
4. fix validated defects rather than normalizing them into tests;
5. use Codecov after CI to measure the resulting production-only improvement;
6. record remaining high-risk gaps rather than padding the change with
   low-value tests.

The first slice targets the FUSE callback layer because it combines the lowest
coverage with a critical semantic boundary. The existing `FuseReplyCapture`
test boundary validates callback argument ownership across fiber admission,
errno translation, structured replies, file/directory lifecycle, and lookup
lifetime without adding production test hooks.

The second slice targets Redis transaction contracts where corrupt metadata or
backend state must fail closed before namespace mutation. In particular, the
private chunk-index interface validates its public inputs and propagates Redis
type errors, while directory moves reject a corrupt parent cycle without
detaching the source or publishing the destination. These are deliberately
tested through `RedisMetaTxn`'s real transaction boundary rather than by
exposing private helpers for tests. Service-backed execution remains a GitHub
CI responsibility.

Storage error paths follow after the metadata transaction slice is reconciled.

## Defects discovered by coverage work

The in-memory metadata audit found a real backend-contract defect while adding
`Readlink` coverage. `RedisMetaImpl::Readlink` rejects a null output pointer
with `EINVAL`, while `MemMetaImpl::Readlink` previously dereferenced that
pointer after successfully resolving a symlink. The behavior-validating
regression test was submitted first and produced the required Draft-PR RED
evidence in GitHub CI (Issue #300); a focused local run also terminated with
`SIGSEGV`. Only after that evidence was captured was the production null check
added. The same tests now validate normal target round-tripping and rejection
of non-symlink/missing inodes, so the fix is anchored in the public metadata
contract rather than the crash alone.

This is the intended outcome of the coverage program: uncovered code led to a
production correctness defect, and the production behavior was corrected
instead of weakening the test or encoding the crash as expected behavior.

The project-level coverage percentage must not regress during this work. The
existing per-file patch coverage gate remains independently required for any
changed C/C++ production file.

## Production source coverage floor

Issue #302 extends the coverage program from an aggregate project signal to a
per-source baseline: every production `src/**/*.cpp` file should reach at least
80% line coverage unless a concrete file-specific exception is justified. The
floor does not change the test-construction contract above. A source file is
not considered remediated by tests that merely execute uncovered lines, encode
an implementation accident as expected behavior, or introduce production
surfaces whose only consumer is a test.

The first bounded slice under this floor is Issue #303 for metadata backends.
Its baseline on `c39a82d` is 52.77% for `mem/VolumeFile.cpp`, 69.88% for
`redis/RedisMetaTxn.cpp`, 73.68% for `redis/RedisBackendContext.cpp`, and
76.47% for `redis/RedisDirIterator.cpp`.

The metadata slice treats the remaining lines as missing contract evidence,
not as a list of statements to execute. In particular:

- `VolumeFile` tests cover persistence input/error behavior through the real
  filesystem boundary, including null output, invalid volume paths, overwrite
  behavior, and write failures, without replacing filesystem calls with test
  doubles;
- `RedisMetaTxn` tests exercise public transaction preconditions and metadata
  invariants such as parent identity, inode type, rename/exchange state,
  hard-link restrictions, and chunk descriptor identity through the existing
  Redis-backed transaction boundary;
- `RedisDirIterator` tests validate output arguments and the Peek/Advance state
  machine rather than reaching into its private cache;
- `RedisBackendContext` tests validate its explicit lifecycle contract: the
  thread-domain owner must shut the backend down before destruction, and
  clients/executors are unavailable after shutdown.

Review of the `VolumeFile` coverage gap removed a redundant production state
machine rather than adding a test seam: creating the complete per-volume
directory already creates the config root when necessary, so separately
probing/creating `/etc/swordfs` duplicated filesystem behavior and introduced
extra host-permission-only branches. The implementation now has one directory
creation path with the same success/failure contract.

`RedisBackendContext.cpp` has a narrow instrumentation limitation around
fatal ownership guards. The destructor and post-shutdown access contracts are
validated with death tests, while normal access, idempotent shutdown, and
successful owner shutdown are validated in-process. Because fatal `CHECK`
branches terminate the death-test subprocess before gcov data is flushed,
Codecov can retain those guard lines as partial/missed even though the behavior
is explicitly verified. No test-only escape hatch or weaker ownership contract
should be added merely to make those fatal branches count as fully covered.

Service-backed Redis behavior remains GitHub-CI authoritative. Deterministic
filesystem/unit behavior may be run locally. If these tests expose a contract
violation in production code, the defect is handled as a normal C++ bug fix
with test-first RED/GREEN evidence instead of changing the assertion to match
the faulty behavior.
