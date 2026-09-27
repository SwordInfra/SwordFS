# Bounded test contracts (#359)

## Purpose

This follow-up closes two gaps left after #355: finite scenario sizes were
still being used as proxies for internal batching policy, and some
event-driven concurrency tests could still wait forever when their expected
completion event never arrived. The change is test/infrastructure-only and
must not alter production behavior or add production-only observability.

## READDIRPLUS batching contract

The old large-directory test created 1024 entries and asserted that metadata
was fetched in more than one call. That only proved bounded batching while
the production width remained below 1024.

The replacement uses a test-only lazy iterator that does not naturally reach
end-of-directory. Its first metadata batch fetch returns a unique sentinel
error. Reaching that error proves that READDIRPLUS voluntarily stopped
collecting candidates and issued a finite metadata batch; the test never
needs to know whether the width is 128, 2048, or another tuning value. The
iterator has a high entry-count guard solely to keep a regression that removes
batching from allocating without bound. That guard is a test resource
watchdog and is not asserted as a production limit.

## Reclaimer multi-pass contract

The old worker test manufactured 1024 real pending deletes and inferred that
the production batch width was smaller than the backlog. The replacement
metadata double returns `has_more=true` on its first pending-delete scan and
records the ordering of the orphan scan and the next pending-delete scan.
This directly forces the protocol condition that requires another worker pass
without encoding the production batch width.

The test still requires the second scan to start before the periodic five-
second safety fallback could be responsible. This timing relationship is
intentional: the behavior under test is specifically `has_more -> Wake()`.
Without exposing a private worker semaphore, completing before the fallback
timer is the externally observable distinction between explicit self-wake and
periodic recovery. The four-second bound is therefore a mechanism-level
liveness contract, not a batching/tuning proxy.

## Bounded async observation

Observer-side waits introduced or touched by #357 use a shared five-second
test watchdog. Producer/control-side waits that deliberately keep work blocked
remain unbounded inside the scenario, but timeout paths release those control
batons before teardown waits on the worker.

The Release-only `ShutdownFromDriverFiberDefersJoinWithoutDeadlock` case is
different: invoking cleanup on the same `FiberRuntime` after the watchdog
fires can deadlock on exactly the defect the test is meant to detect. The case
therefore runs in a death-test subprocess. A failed one-second semantic
watchdog exits the child immediately without RAII cleanup, and a longer child
alarm bounds any unexpected hang after the observed shutdown returns.

The unit-test runner also has a ten-minute outer `timeout`. Recent successful
Debug/Release CI build-and-test jobs complete the full build plus unit-test
phase within several minutes, so this leaves substantial headroom while
preventing any missed barrier from consuming the entire GitHub job timeout.

## Repo-wide scan boundary

The #359 scan also found unbounded EventBase/Baton/thread-completion barriers
outside the #357-touched surface, notably in `ChunkTest`, `DirHandleTest`,
`FileReadWriterTest`, `FileHandleTest`, and the shared `FiberTest.hpp` helper.
Those cases require a broader lifetime/cleanup review and are tracked by #361
rather than being mechanically rewritten in this focused follow-up. The outer
runner timeout provides an immediate process-level safety net meanwhile.

## #355 verification-policy clarification

The current engineering workflow forbids compiling SwordFS or running compiled
SwordFS unit-test binaries on the local development server. Focused compilation
and unit-test execution use the GitHub Dev Build path; integration/E2E/FUSE-
service-backed/recovery/privilege-sensitive execution remains formal GitHub-CI-
only. The #355 audit document is clarified to state that boundary explicitly.

## Verification

This change is expected to touch tests, test tooling, and design documents
only. No production C/C++ patch coverage is therefore expected to apply.

Verification requires:

1. use GitHub Dev Build to compile `swordfs_test`; do not compile SwordFS locally;
2. run focused FiberRuntime, READDIRPLUS/VfsImpl, and Reclaimer unit tests through GitHub Dev Build;
3. run `bash -n scripts/testing/run-ut.sh`, `pre-commit run --all-files`, and
   `git diff --check`;
4. run focused GitHub Dev Build and the full required PR CI matrix;
5. perform a requirement-first readability/test-quality/simplicity review and
   reconcile #359's acceptance criteria against the final diff.

## Non-goals

- changing production batch widths or safety-scan timing;
- adding production APIs solely for test observation;
- replacing event coordination with sleeps;
- weakening retry/publication/POSIX contracts that intentionally remain exact;
- absorbing #361's broader concurrency-test lifetime cleanup into this PR.
