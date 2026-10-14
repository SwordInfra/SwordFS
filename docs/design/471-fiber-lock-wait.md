# #471 — Bounded Fiber-domain wait/retry for POSIX and BSD flock

**Implementation checkpoint:** #475 now includes test-first C++ Fiber wait/retry,
read-only Redis probes and Mount lifecycle changes; formal verification and
independent review are pending. Remote FUSE lock capabilities remain disabled. Parent #433 under #379. Prerequisites #428–#432 merged.
Follow-ups: #472 FUSE interruption; #473 BSD flock remote activation;
#474 traditional POSIX remote activation subject to #464 OFD transport gate.

## 1. Problem, exact boundary and peer evidence

The existing shared `FuseAdvisoryLocks` adapter executes a **single**
thread-domain `ILockAuthority::SetPosix` or `SetFlock` attempt through
`BlockingExecutor::RunFromFiber`. #430 returns `EAGAIN` on a real conflict,
and already owns Redis WATCH/EXEC, lock identity, lease checks and ambiguous
commit retry/error-only behavior. `SwordFsSetlk(sleep=1)` currently returns
`EOPNOTSUPP`; `FuseAdvisoryLocks::Flock` rejects blocking SH/EX. Both FUSE
remote lock feature flags remain disabled. #471 changes **waiting behavior**,
not #430 grant authority or feature negotiation.

Source-checked peer designs:

- [JuiceFS Redis `Setlk`/`Flock`](https://github.com/juicedata/juicefs/blob/eba86e8a7bb054ad47a15a05b1cf144d0b3da0ec/pkg/meta/redis_lock.go)
  retries a failed nonblocking Redis transaction after short sleeps and
  checks request cancellation. Its current fixed 1ms write / 10ms read
  delays are *not* SwordFS performance defaults; a large waiter population
  amplifies Redis transactions.
- [CephFS MDS lock manager](https://github.com/ceph/ceph/blob/7d526f4aeb41798c2e912f2b3a102e5bd28107b0/src/mds/flock.cc)
  and [GlusterFS lock service](https://github.com/gluster/glusterfs/blob/2326b1da07dc9a424aa798b9a81ea3b2a17dd997/xlators/features/locks/src/common.c)
  maintain server-side waiting requests because they have a dedicated
  stateful server. SwordFS has no analogous MDS waiter service.
- [NFSv4.1 RFC 8881 §9.6](https://www.rfc-editor.org/rfc/rfc8881.html#section-9.6)
  makes lock-availability notification optional: a notification never
  substitutes for an authoritative retry. Avoid a notification/version
  protocol before real Redis-load evidence justifies it.
- [Folly `EventBase`/`HHWheelTimer`](https://github.com/facebook/folly/blob/main/folly/io/async/HHWheelTimer.h)
  already provides a shared timing wheel. Do not implement a custom wheel.
  SwordFS's `utils::FiberBaton` can hand wakeups between thread domains.

## 2. Required observable behavior

1. An ordinary **nonblocking** request (`F_SETLK` or `LOCK_NB`) still makes
   one authority attempt and returns existing results unchanged. A valid
   `F_UNLCK` / `LOCK_UN`, `GETLK`, `ENOLCK`, `EIO`, invalid input or stale
   lease does **not** enter the wait loop.
2. A blocking `F_SETLKW` or `flock(LOCK_SH/LOCK_EX)` retries only **genuine
   lock conflicts (`EAGAIN`)**. Each iteration submits a new bounded
   authoritative attempt, with the same inode, mount generation, opaque
   owner, requested type and byte-range PID metadata. Compatible lock
   classes continue to coexist. Successful grant remains gated by #429's
   `CanGrant()` immediately before the FUSE reply in the existing hook.
3. **Blocking is allowed to last indefinitely** until grant, request
   cancellation or genuine service failure. User-initiated umount may
   terminate the Mount process with pending requests. "Bounded wait" means
   bounds on retry rate, worker occupancy, waiter count and memory, **not**
   a new application-visible acquisition timeout.
4. No strict FIFO, cross-owner precedence or new ordering of overlapping
   SETLK/FLUSH calls is promised. Respect Linux POSIX/flock semantics,
   including the distinction between ordinary acquisition and **conversion
   of an already-held flock**.
5. Blocked requests register with one logical `LockRetryScheduler` per
   Mount. At each tick it **reads** current authoritative lock state for the
   contended `(inode, lock class)` groups, determines which request(s)
   **might** acquire their lock based on that snapshot, and wakes only those
   requests. The Scheduler **never grants, reserves, converts or unlocks a
   lock**. Only a request's existing #430 `SetPosix`/`SetFlock` mutation can
   grant the lock. A competing request or Mount may win after the read;
   `EAGAIN` simply sends the awakened request back to sleep. The registry
   lives only in the client process, not in Redis.

## 3. Mount-level retry scheduler (no independent per-lock poll timers)

**Design selection following owner feedback:** prefer one *logical*,
Mount-owned `LockRetryScheduler` shared by traditional POSIX and BSD flock
requests. "One scheduler" means one bounded registration/limit owner,
not a new OS thread or Redis worker pool. The Scheduler periodically makes
**read-only** queries for waiting inode/lock-class groups and evaluates
candidate conflicts locally. It does **not** invoke the mutating lock
methods, acquire a lock on behalf of a request, reserve a grant or mutate
Redis lock ownership. The authoritative #430 `SetPosix`/`SetFlock`
operation remains exclusively in the original request Fiber after wakeup.
A shared tick without grouped reads and exact wakeups would not lower load.

There are two waits: Linux blocks the **application syscall** until its
FUSE request receives a reply; SwordFS suspends the **request Fiber**, not
the FUSE dispatcher or a Redis worker. Each first attempt remains immediate.

```text
FUSE blocking SETLKW / flock -> submit request Fiber
  -> immediate atomic SetPosix/SetFlock via BlockingExecutor
  -> success / non-conflict error -> one FUSE reply
  -> genuine EAGAIN:
       return worker/Redis connection completely
       register wait with Mount LockRetryScheduler
       suspend request Fiber on its own FiberBaton

Shared scheduler timer tick (only while waiters exist)
  -> group pending requests by (inode, lock class)
  -> enqueue coordinator Fiber in an EXISTING FiberRuntime
  -> coordinator Fiber uses existing BlockingExecutor for bounded
     READ-ONLY Redis lock-state probes on EXISTING workers
  -> snapshot: compare each waiting request against other valid Owners,
     using exact POSIX range/type or BSD SH/EX conflict semantics
  -> select compatible, potentially grantable requests, with fairness cap
  -> post only each selected request's own Baton (no broadcast)

Awakened request Fiber
  -> check its #477 request cancellation token
  -> fresh atomic SetPosix/SetFlock through existing BlockingExecutor
  -> if another Owner won in the meantime: EAGAIN -> register/sleep again
  -> otherwise return actual success/non-conflict error to FUSE reply owner
```

Pseudocode for semantics only (not an approved C++ API):

```cpp
auto status = TryLockThroughBlockingExecutor();  // Immediate first try.
while (is_blocking && status.ToErrno() == EAGAIN) {
  if (request_cancelled) return Cancelled();

  utils::FiberBaton wakeup;
  auto registration = scheduler.Register(inode, lock_class, request, wakeup);
  if (!registration.ok()) return NoLocks();
  wakeup.wait();  // Exact wake after a read-only probe (or request cancel).
  auto reason = registration.TakeWakeReason();  // Own a result copy.
  scheduler.Unregister(registration);

  if (request_cancelled) return Cancelled();
  if (reason.probe_error) return reason.error;
  status = TryLockThroughBlockingExecutor();  // Sole atomic grant path.
  // EAGAIN from competing grants simply loops back into the scheduler.
}
return status;
```

The actual implementation must use safe RAII registration and
cancellation/teardown ownership; the sketch omits synchronization and is
**not** a license to store raw Baton pointers across unprotected callbacks.
The probe result is only a **possibly available** signal, not a claim that
an existing Holder has been unlocked or that a lock was granted. The
request must always perform the complete original #430 atomic mutation.
Tick selection, individual request cancellation and registration teardown
must share a safe lifetime contract. While the Mount remains running, only
the owning request Fiber replies to its FUSE request. Voluntary process exit
does not broadcast request cancellation or require every Fiber to reply
(see Section 4).
Only **EAGAIN from a real conflicting lock** is eligible for scheduling;
other Redis/backend failures are returned to the affected request without
unmount. Once an attempt is complete, its Redis worker is available to
handle unrelated unlocks/nonblocking calls before this Fiber sleeps.

### 3.1 Read-only probe, local compatibility and exact wake

**Batch the reads first, not the grants.** At each shared tick, group
pending requests by `(inode, lock class)` because POSIX record locks and
BSD flock occupy separate Redis Hashes. For each eligible group, perform
**one validated read-only lock-state probe**, rather than an independent
Redis query/transaction per waiter. Use existing #430 Hash decoding,
Owner-generation qualification and lease filtering, on the existing
`BlockingExecutor` thread domain. This may require a **narrow read-only
inspection API** for lock groups (BSD flock currently has no `GetFlock`
probe; `GetPosix` is a per-request lookup and can trigger lazy stale-owner
cleanup, so it cannot itself be assumed to be a pure batched snapshot).
Share the existing decoding and valid-lease filtering helpers but do not
change the persistence schema or the grant, conversion, release and
transaction semantics. Avoid exposing unvalidated raw Redis state to the
FUSE layer. Inspection must not write or invoke lazy GC as a side effect.

The Scheduler compares each waiting request's exact opaque Owner,
requested type and POSIX byte range against the probed valid Holder
snapshot. It may select more than one request when their requested locks
are mutually compatible (e.g. non-overlapping record ranges or SH readers).
For conflicting writers, select only a bounded number of candidates,
**notify only those individual request Batons**, and rotate candidates
across ticks. The Scheduler must avoid local starvation, but promises no
strict FIFO or cross-Mount fairness. If a read-only probe cannot confirm
availability, wait until the next tick; do not accidentally grant.

**The check-to-acquire race is explicitly acceptable.** Another Mount may
change the locks between the Scheduler's probe and the awakened request's
`SetPosix`/`SetFlock`. Only the latter can grant. A conflict there returns
`EAGAIN` and the request re-registers; no reservation, CAS token, special
Scheduler ownership or conditional rollback is necessary. Likewise,
remote lock release immediately after a probe observed a conflict is
caught by a later tick; correctness does not depend on a release
notification. If a probe returns a genuine Redis/lease error, wake the
relevant waiting requests **with that error** instead of silently leaving
an indefinite waiter while the backend is down. As always, do not unmount.

**No thundering herd:** one `utils::FiberBaton` per blocked request, one
wake target per eligibility decision; waking all waiters for one inode is
not an acceptable shortcut. `Baton` is the appropriate installed
**fiber-aware** primitive; a POSIX-thread `std::condition_variable` would
block a driver thread. The currently installed Folly Fiber implementation
has `folly::fibers::Baton` (already aliased by SwordFS), which provides
single-waiter `wait()` and thread-safe `post()`; it does not require a
custom Fiber condition variable.

### 3.2 Query cost versus grant cost

If `N` waiters on the same group independently call SetPosix/SetFlock at
interval `T`, the authority sees approximately `N/T` conflict transactions
per second. With read-only group probes, the Scheduler instead makes
approximately `G/T` probe groups per second for `G` active groups plus
**only the actually woken requests'** existing atomic grant attempts.
The actual Redis cost of one group probe includes its Hash and Owner
Lease validation; it is not free, and there is no claim of measured
savings until tests record commands, CPU and network round trips. Group
reads are allowed to be internally pipelined where safe but **a multi-
Owner batch mutation is neither proposed nor needed**. Redis Pipeline
can reduce RTT but does not itself create atomic lock grants.

A strict one-query-per-tick total rule may delay independent inodes;
therefore bound concurrent group probes and candidate wakeups under a
Mount-wide budget. Initial requests and all nonblocking/unlock operations
bypass the Scheduler entirely. Choose the tick frequency and in-flight
limits from measured latency/load, not JuiceFS's fixed sleep values.

### 3.3 Folly EventBase/FiberRuntime ownership

**Use the existing Redis worker pool; do not keep a second lock pool.**
`RedisMetaOps` already owns `RedisBackendContext`, which owns the
`swordfs-redis` `BlockingExecutor` configured by `meta_thread_count`.
The current Mount creates an independent `BlockingExecutor(2,
"swordfs-lock")` for `FuseAdvisoryLocks`; `2` is a hardcoded initial
choice, not a benchmark-derived concurrency target. #471 should inject
the existing Redis executor into the lock adapter through a narrow,
lifetime-safe ownership/access seam, without exposing raw Redis storage
or coupling `RedisLockAuthority` grant semantics to metadata transactions.
Keep the authority's distinct lock/lease semantics and error contract.
Because foreground metadata and lock requests will share workers, bound
the number of scheduled read-only lock probes and their queue pressure:
probes must not flood the pool and starve foreground metadata or unlocks.
No new priority scheduler, configurable lock pool or thread-count tuning
is required absent measured contention (deferred investigation #481).

Reuse Folly's existing `EventBase::timer()` / `HHWheelTimer`, **not** a
hand-rolled timing wheel or thread-per-lock polling loop. The timer
callback submits a **coordinator Fiber** into an existing FiberRuntime;
it never calls Redis, executes `RunFromFiber` in the EventBase main
context, or blocks the EventBase. That coordinator Fiber submits the
read-only probes through the existing `BlockingExecutor::RunFromFiber`
while respecting one bounded in-flight tick/probe budget. Probe completion
selects and posts per-request Batons without holding a worker while any
request waits. SwordFS currently has **one FiberRuntime/EventBase per FUSE
worker thread**: a single logical Mount scheduler therefore needs correct
cross-EventBase callback dispatch and lifetime, using the existing
`runInEventBaseThread` / `FiberBaton::post` primitives. Do not call an
EventBase timer from a foreign thread. If a physical single-EventBase
timer proves awkward, sharding *timer callbacks* per FiberRuntime while
maintaining one Mount-wide admission/retry budget is acceptable; creating
an extra blocking thread pool solely for the scheduler is not.

Bound actual waiting registrations per Mount; if exhausted, return an
explicit resource error such as `ENOLCK`, rather than blocking a FUSE
dispatcher or dropping/replying to the wrong request. Timer resources are
inactive when the waiting set is empty. No Redis Pub/Sub, inode version
counter, server-side queue or custom cross-Mount deadlock graph is needed.

### 3.4 Consume #477 request cancellation; no Mount-wide request cancellation

The **Scheduler grants nothing**; pending-wait cancellation is local,
without Redis undo or special lock Owner mutation. #477 already provides
the generic `folly::CancellationSource` per FUSE request, a copied
`folly::CancellationToken` in `SwordFsContext`, the libfuse interrupt bridge,
and `FiberRuntime::Post()`. #471 must consume this token; it must **not**
introduce separate request-cancel state, `CancelWaiter()`, or another
libfuse interrupt-registration lifecycle. The blocking **lock operation**
registers its own `folly::CancellationCallback` and arranges an
execution-domain-safe waiter removal and Baton wake. `FiberRuntime::Post()`
runs on the EventBase driver thread **outside** a Fiber; thread-safe Baton
posting is permitted there, but Fiber-only operations are not.

The active request registration needs a stable lifetime (for example, a
small shared waiter state containing a FiberBaton and a completion reason,
plus an RAII registration handle). It supports these independently safe
operations without a new global request journal:

1. **Register/Wait:** check the cancellation token **atomically with** publishing
   the waiter. A cancellation preceding registration must be observed,
   without an infinite wait. A `Baton::post()` preceding `wait()` remains
   visible. On subsequent retries, use a fresh Baton or reset only after
   all possible posters have quiesced; the Folly API does not allow a reset
   racing with a post.
2. **NotifyAvailable:** after a read-only probe, publish only a *possible*
   availability result and wake that individual request. It never grants
   or reserves a lock. Requests already cancelled/unregistered are ignored.
3. **Request cancellation:** the lock operation removes its waiter and wakes
   its Baton when its token is cancelled, without waiting for a read-only
   probe. The probe may finish later but cannot post into retired state,
   recreate the waiter or cause another TryLock. This is idempotent.
4. **User-initiated umount:** this is **not** request cancellation.
   No runtime lock error or blocked request may initiate umount.
   #471 retains original Mount cleanup (Section 4).

**Critical distinction: cancellation of a wait vs cancellation of an
in-flight Redis grant attempt.** During `WAITING`, no grant operation runs;
signal cancellation can just terminate the wait. After `NotifyAvailable`
and before submitting the next SetPosix/SetFlock, the request Fiber checks
the token again; if cancelled, it returns EINTR and **must never
submit** a new TryLock. If cancellation races with an **already submitted**
Redis transaction, neither Scheduler nor interrupt callback may reply
`EINTR` immediately. The original Fiber must first receive the transaction
result: on a confirmed grant, a *late* signal may lose to successful grant
and a normal success reply as verified in #428; on `EAGAIN`/other **known
ungranted** result, it may return `EINTR` rather than re-enter the
Scheduler. Do not
reply `EINTR` while an outstanding operation might subsequently grant a
lock. Keep #430's existing error-only contract for unresolvable backend
outcomes; the Redis commit and FUSE reply are not an atomic operation.

The request Fiber **alone** issues **exactly one** normal-operation FUSE
reply after retiring any in-flight Redis operation. The #477 reply adapter
already unregisters the libfuse interrupt callback before consuming the
request. The Scheduler publishes only availability hints and probe errors;
the lock operation handles cancellation using the token. #472 owns the
lock-specific signal/EINTR/grant race, not a second generic FUSE interrupt
bridge. #471 tests the lock operation's cancellation registration, exact
wake and waiter lifetime. No per-inode condition-variable broadcast, polling
threads, Redis cancellation records or speculative compensation.

## 4. User-initiated unmount boundary

**Owner policy (2026-10-15):** SwordFS must **never initiate umount
automatically** for Redis outages, Owner Lease loss, lock conflicts,
resource pressure, blocked requests, cancellation, or any other lock
error. Such events affect only the requesting operation, while FUSE
and ordinary I/O continue. Unmount is a user/operator action and is
not a lock cancellation mechanism.

This PR keeps the existing FuseSessionGuard / SwordFsDestroy /
RunMount cleanup path, including ShutdownFiberRuntime and active Owner
Lease revocation. It **does not** introduce a process-wide _Exit(0)
shortcut or change FUSE event-loop completion to mean user-initiated
umount. That earlier #475 change was reverted: a zero return from the
libfuse session loop does not establish why it terminated, and broad
process exit skips established runtime and volume cleanup.

**Activation boundary:** #471 leaves FUSE_CAP_POSIX_LOCKS and
FUSE_CAP_FLOCK_LOCKS disabled. During later #473/#474 integration,
revisit user-initiated unmount if the kernel can leave a request Fiber
waiting indefinitely. The future design must avoid coredumps and must
not introduce automatic umount or broadcast cancellation to all
waiters. A narrowly scoped exit strategy, if necessary then, is a
separate decision and is not preemptively installed by #471.

The previously proposed mandatory stressed umount test involving a
blocked waiter, concurrent probe and multiple EventBases was withdrawn
as a reviewer-added requirement not explicitly requested by the owner.
Existing user-initiated unmount and lease-revocation tests remain
relevant.

## 5. One semantic gate: flock SH -> EX conversion

Do not assume a retry loop around #430's current `SetFlock` preserves all
Linux flock semantics. `RedisLockAuthority::SetFlock` currently keeps an
owner's existing SH while a conflicting owner's SH prevents an EX upgrade.
If two owners each hold SH and both attempt **blocking** EX conversion,
simply retrying `EAGAIN` while each retains SH can leave both blocked even
without any active owner able to release its lock. [Linux `flock(2)`](https://man7.org/linux/man-pages/man2/flock.2.html)
describes conversion as **not guaranteed atomic**: the existing lock can
be removed before the new mode is established.

**Mandatory bounded test/design check before #471 implementation:** compare
two concurrent SH -> EX conversion outcomes against the target Linux flock
path (and the #432 current nonblocking conversion behavior). If the retained
SH is a real incompatibility, address **only** the smallest conversion
semantic change necessary in the existing #430 authority/adapter within
the task's implementation or explicitly route an independent #432
regression to its own Issue; do not solve this with FIFO queues or a
distributed deadlock detector. This is a standard flock behavioral gap,
not a request for stronger-than-POSIX ordering.

## 6. TDD, CI and capability boundaries

| Behavior | Test-first acceptance |
| --- | --- |
| Basic waiting | Owner A holds a conflict, B waits under the shared Mount scheduler and eventually succeeds after A unlocks; both lock classes |
| Compatibility | Two SH readers grant immediately; conflicting SH/EX and range conversions remain authoritative |
| Failure differentiation | `EAGAIN` retries only in blocking mode; invalid input, `ENOLCK`, lease expired, Redis `EIO` and ambiguous EXEC error do not retry forever and **never unmount FUSE** |
| Worker availability | With more waiting fibers than shared Redis worker threads, normal metadata and independent lock/unlock requests can still progress; waiting Fibers hold no workers and background lock probes cannot saturate the shared queue |
| Shared read-only inspection | Grouped inode/class probes do **not** call `SetPosix/SetFlock`, change Redis locks or wake requests when the snapshot still conflicts; confirm Owner Lease validation, request-specific ranges, SH compatibility and exact Baton wake |
| Cancellation seam (#471) | Use the #477 request cancellation token before registration, between registration and `wait()`, during read-only probe, and before a retry; exact wake, no re-registration or new grant, safe shared-state lifetime; no scheduler-owned cancellation state |
| In-flight cancel (#472) | Cancel while original Fiber's Redis TryLock is executing: never reply `EINTR` before its outcome is known; confirmed success may win a late signal; no post-EINTR grant or double reply |
| Probe-to-grant race | Another Mount wins after `Probe`/`notify`: awakened original request receives `EAGAIN` from its normal atomic SetPosix/SetFlock, re-registers without ghost locks or compensation |
| Grouped scheduling | Many same-inode conflicting waiters do **not** all run a Redis transaction per tick; independent ranges and compatible SH still progress; rotating candidates do not starve |
| Cross-EventBase lifetime | Multiple FUSE driver EventBases can register/wake/unregister without use-after-free, races or an extra thread pool |
| Contention cost | Compare one read-only probe per hot `(inode,class)` group plus candidate grants with per-waiter tries; measure Redis commands/CPU and p95/p99 latency, avoiding brittle exact retry-count assertions |
| Unmount boundary | Only user/operator action initiates umount; lock/Redis failures never do. #471 retains original Mount cleanup; #473/#474 revisit active kernel lock wait teardown only when enabled |
| Conversion | Two concurrent SH -> EX upgraders do not become permanently stuck solely because each still holds an old SH; compare native Linux semantics |
| Request cancellation | A #477 cancellation token terminates a sleeping lock waiter while Mount stays active; #472 later proves lock-specific early/during/late interrupt outcomes and `EINTR` |
| Nonactivation | Existing `FUSE_CAP_POSIX_LOCKS` / `FUSE_CAP_FLOCK_LOCKS` remain disabled; service-backed adapter tests do not claim negotiated remote locking |

For C++ feature work, first establish contract **RED** on a dedicated CI
branch, then implement and validate **GREEN** via GitHub focused/full CI;
don't compile locally. Use Barriers/Batons for test setup/completion,
not wall-clock sleeps to infer correctness. Validate each touched C++
production file has >90% patch coverage. No new ordinary data-I/O Redis
transactions or generic lock/session service is permitted.

**Design-review disposition:** prefer a logical **per-Mount
`LockRetryScheduler`** with shared Folly timing, **one read-only probe per
active inode/lock-class group** when scheduled, and exact wakeup of only
possibly eligible request Fibers. The **original request** performs the
only atomic #430 `SetPosix/SetFlock` authorization. A race simply yields
`EAGAIN` and another wait; no Scheduler-side grant, reservation, rollback
or mutation is allowed. No batch-grant API, Pub/Sub, custom wheel or
persistent server queue. Cross-EventBase wakeup/cancellation, compatible-
range progress, probe errors and two-Owner flock conversion are mandatory
scoped implementation tests. User-initiated unmount with active kernel
lock waits belongs to capability activation (#473/#474). #472 owns FUSE
signal cancellation; #473/#474 own actual remote capability activation.
This is design only, not C++ or measured performance evidence.

## Implementation integration checkpoint (2026-10-14)

- One logical Mount scheduler admits a bounded population of waiters and
  multiplexes read-only holder/lease probes by `(inode, class)`. Probe work is
  serialized (at most one Redis worker active per probe) with a small rotating
  group budget per tick; independent foreground Redis operations bypass it.
  Original request Fibers alone own SetPosix/SetFlock and FUSE replies.
- `RedisMetaImpl` exposes its existing `RedisBackendContext` executor to the
  Mount lock adapter; the second hardcoded two-thread lock pool is removed.
- Cancellation follows #477: the waiter Baton and its notification state are
  shared across callback/timer lifetimes; the request Fiber unregisters the
  waiter and owns any EINTR result, with confirmed in-flight grants permitted
  to win a concurrent cancellation.
- A conflicting flock mode conversion releases that Owner's old mode inside
  the same WATCH/EXEC transaction before reporting EAGAIN; Linux native
  two-upgrader behavior supports this correction. The lock authority still
  never grants speculatively from a scheduler read.
- No new process-wide Mount exit path is included in #471. The existing
  session destruction, FiberRuntime shutdown and active lease revoke
  sequence remains intact while remote lock capabilities stay disabled.
  Lock and Redis failures never initiate umount.
- First valid RED: Dev Build #38575691288, with compilation succeeding and
  `BlockingFlockAcquiresWithoutContention` failing on EOPNOTSUPP. The initial
  aborted fixture execution #38574699620 was corrected and is not RED proof.

This checkpoint describes an implementation awaiting full CI, coverage,
concurrent cancellation/unmount integration evidence and human review; it
must not be treated as activation of remote POSIX/flock capabilities.

## Review correction: no production exit exception in #475

An earlier #475 iteration introduced _Exit(0) for every successful
libfuse event-loop return and exempted the Mount.cpp file from the
per-file coverage gate. Neither change existed on main. The review
removed both as broader, unnecessary lifecycle and CI exceptions.
The original Mount cleanup path remains in place.

Redis metadata executor wiring introduced in Mount.cpp remains subject
to the ordinary coverage and verification requirements. If CI reveals
a new uncovered branch, address its actual testability or reachability;
do not reinstate an exemption on the obsolete _Exit rationale.

## Measured bounded contention cost (2026-10-15, PR #475)

The Redis-backed test
`RedisLockAuthorityTest.GroupedReadOnlyProbesReduceRedisCommandCostUnderContention`
ran in focused GitHub Debug Dev Build
[38676767068](https://github.com/SwordInfra/SwordFS/actions/runs/38676767068)
and passed. On one CI-host Redis instance, owner A held an exclusive BSD
flock while 16 other owners remained in conflict on the same inode. For
24 observation rounds we compared rejected individual authority TryLock
operations (16 per round) with one validated `RedisLockAuthority::Inspect`
per `(inode, lock-class)` group per round. `INFO stats`
`total_commands_processed` gives actual Redis-side command deltas; CPU
was sampled using process `std::clock`, and round durations used
`std::chrono::steady_clock`. Sample output:

| Measurement | 16 per-waiter retry attempts/round | 1 grouped inspection/round |
| --- | ---: | ---: |
| Redis commands over 24 rounds | 3,841 | 265 |
| Process CPU time (ms) | 82.813 | 4.315 |
| p95 round latency (ms) | 15.932 | 0.791 |
| p99 round latency (ms) | 16.048 | 0.794 |

Grouped inspection reduced observed Redis commands by **93.1%** in this
controlled **same-group** comparison. The comparison is between whole
rounds of 16 mutating lock attempts and a single read-only snapshot,
not an assertion that a single Inspect is semantically a grant. CPU and
latency are diagnostic measurements, not hard CI thresholds.

The same test separately sampled 24 **actual blocking-Fiber grants**, each
following an external owner's release. A 70ms warm-up hold was used to
prime the blocked request for measurement, **not** as a wait-state
correctness assertion: completion of the originating Fiber was required.
Post-unlock grant completion latency measured **p95 32.423ms / p99
36.240ms**. These values include the releasing Redis command and scheduler
notification/retry; the scheduler's configured tick is 50ms. They are
one representative CI environment sample, not a cross-environment SLA or
10K-waiter scaling claim. Higher contention, multiple groups and priority
tradeoffs remain #481/#482 work; future measurements should rerun this
representative test rather than relying on these fixed numbers.

## Coverage attribution correction for declarative C++ type lines (2026-10-15)

The strict Codecov per-file review of #475 found two header lines reported
as executable despite being **only C++ type declarations**: the opening
`struct Waiter {` line in `src/fuse/LockRetryScheduler.hpp` and the opening
`struct LockHolder {` line in `src/metadata/LockAuthority.hpp`. Before
this correction each header scored 2/3 = 66.67%, while the headers' two
actual measured executable lines were fully covered.

Meaningful independent tests already instantiate default Waiters, inspect
Baton/atomic notification state, construct LockHolders, and copy their range
snapshots without shared mutation (`LockRetrySchedulerTest`). Those tests
passed focused CI but gcov continued to associate a zero-hit implicit
compiler-generated special member with each **declaration** line. This
represents source-line attribution rather than an uncovered executable
production branch. A single `LCOV_EXCL_LINE` annotation on each
**non-executable declaration** is therefore used instead of file-level
exceptions or reduction in the >90% threshold. No member initializers,
constructors, scheduler methods, Redis inspection, Mount logic, or actual
branch instructions are excluded. In particular, `Mount.cpp` now has 2/2
100% measured patch lines after removing its redundant post-validation
RTTI assertion, with no coverage exception. Verify Codecov's per-file
percentages again on the next head before accepting this resolution.
