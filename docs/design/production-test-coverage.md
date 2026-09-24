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
