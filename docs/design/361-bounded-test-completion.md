# Bounded unit-test completion barriers (#361)

## Purpose

SwordFS concurrency tests deliberately block producer-side operations to expose
ordering, serialization, and shutdown behavior. Those control-side blockers are
part of the scenario. The observer side, however, must never wait forever for a
signal whose absence is exactly the regression under test.

This change extends the bounded-test contract introduced by #359 to the
remaining high-risk unit-test completion barriers. It is test-only: production
behavior and production observability APIs are unchanged.

## Completion-barrier contract

Observer-side EventBase and Baton waits use the shared five-second
`TestWatchdog.hpp` deadline. A normal completion therefore has the same
semantic ordering as before, while a missing signal fails close to the owning
test instead of falling through to the ten-minute unit-suite timeout.

Tests with an explicit producer/control blocker follow this cleanup sequence:

1. wait for the observer milestone with the bounded watchdog;
2. if the milestone is missing, report the failed expectation;
3. release every test-owned blocker that a late worker/fiber may reach;
4. bounded-drain the affected completion signal(s);
5. only then allow EventBase, thread, or fixture teardown.

If the bounded drain itself fails, the test process aborts immediately. At that
point the test cannot safely destroy stack state that a still-running fiber or
thread may reference, so process termination is safer than returning through
RAII and risking a second hang or use-after-free.

## Shared fiber helper lifetime

`RunInTestFiber` is a special case because it owns the EventBase that owns the
fiber under test and has no generic way to release an arbitrary blocker inside
the caller's function. A watchdog timeout therefore cannot safely return to the
caller: the unfinished fiber may still reference caller stack or fixture state.

The helper consequently has a fail-stop lifetime contract with a 30-second
whole-fiber deadline. That is deliberately longer than one local observer
watchdog because a fiber test may need a five-second observation failure plus
another bounded interval to release blockers and drain safely.

- drive the EventBase until the fiber posts completion, bounded by the
  whole-fiber watchdog;
- propagate the fiber's exception on normal completion as before;
- if completion is missing, report the watchdog failure and abort the test
  process rather than destroying the EventBase around a live fiber.

When the helper is called from an already-running fiber it preserves that
execution context rather than nesting another EventBase. The owning fiber
driver supplies the lifetime bound; `FIBER_TEST` uses the bounded outer helper
for exactly this reason.

Local copies of the same unbounded helper in Chunk, FileReadWriter, and
BlockingExecutor tests are removed in favor of this one contract. Redis
metadata's value-returning `RunInFiber` adapter executes its callable through
the shared helper and only transports the result.

## Thread completion and joins

A `std::thread::join()` is safe only after the test has positive evidence that
the thread has reached a nonblocking completion point. High-risk tests add a
completion Baton and wait for it through the watchdog before calling
`join()`. The join then only reclaims an already-finished thread.

Producer-side Batons may remain unbounded inside worker code when they are the
deliberate scenario controls. Timeout cleanup posts those Batons before waiting
for worker completion.

`StartFiberTestThread` executes its body through `RunInTestFiber`, so ordinary
finite worker joins using that helper inherit the 30-second fail-stop bound.
Tests where a join is itself part of the deadlock/lifecycle contract additionally
use an explicit completion Baton so they fail at the more precise five-second
observer boundary.

FiberRuntime joins are classified by this rule:

- shutdown threads that execute production shutdown logic require a bounded
  completion Baton before `join()`;
- worker threads are joined only after their final completion Baton is
  observed; cleanup paths post all stage-control Batons first;
- joins after an already-observed thread completion are teardown-only and do
  not need a second independent deadline.

## Synchronization death tests

Synchronization tests use the same shared fiber helper instead of open-coded
EventBase loops. Death tests still assert the same execution-domain violation;
the helper only supplies the bounded fiber lifetime around the operation. No
sleep-based scheduling or weakened death-test expectation is introduced.

## Verification

This task changes test code and test design documentation only, so the
production C/C++ Codecov patch-coverage gate does not apply.

Verification requires:

1. no remaining unbounded observer-side `EventBase` completion loop in the
   #361 target files;
2. every timeout path with a test-owned blocker releases that blocker before
   teardown;
3. high-risk thread joins are preceded by bounded completion evidence;
4. `pre-commit run --all-files` and `git diff --check` pass locally without
   compiling SwordFS;
5. focused GitHub Dev Build compiles `swordfs_test` and runs the affected test
   filters;
6. the full required pull-request CI matrix is green.

## Non-goals

- changing production synchronization, scheduling, shutdown, or filesystem
  semantics;
- adding production hooks solely for test observability;
- replacing deterministic event coordination with sleeps;
- mechanically rewriting finite worker joins that do not depend on a
  production deadlock or test-controlled blocker.
