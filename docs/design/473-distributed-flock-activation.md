# #473 — Activate kernel-negotiated distributed BSD flock

**Status:** Remote BSD flock activation implemented and verified in GitHub CI;
independent review and merge remain pending. Upstream `generic/504` is not
claimed as distributed conformance support.
**Parent:** #433 under #379. **Prerequisites:** #432, #471 and #472 merged.
**Canonical shared architecture:** [POSIX conformance](posix-conformance.md).

## 1. Problem and invariants

Redis owner leases, atomic flock authorization, blocking wait and request
interrupt handling already exist. Before activation,
`VfsHookFactory::SwordFsInit` unconditionally unset `FUSE_CAP_FLOCK_LOCKS`.
Linux consequently handled application flock in the **local kernel**, and the
earlier `generic/504` PASS did not demonstrate cross-Mount exclusion.

**TDD RED evidence (2026-10-15):** GitHub CI run
[`38755908051`](https://github.com/SwordInfra/SwordFS/actions/runs/38755908051)
compiled the first real two-Mount test at `f32d271`. Release and Debug E2E
both observed that Mount B incorrectly acquired EX while Mount A held EX;
they also observed that a default Mount left Redis owner generation at zero.
No production behavior was changed before these failures were established.

**Integration findings (2026-10-15):** The first GREEN run
[`38758074403`](https://github.com/SwordInfra/SwordFS/actions/runs/38758074403)
passed real cross-Mount Release E2E, Debug tests and the Codecov patch gate.
The conformance aggregate exposed `generic/504` as a regression against its
old *kernel-local-only* PASS baseline. Inspection of the pinned upstream test
shows it specifically searches for the inode's **kernel FLOCK entry in
`/proc/locks`** after `flock`. Negotiated FUSE remote flock delegates
ownership to Redis instead and does not populate that local kernel lock
table. That upstream introspection assumption is not applicable to a remote
lock authority; its exact `lock info not found` outcome is tracked as an
explicit `upstream_not_applicable` known gap, **not** promoted to supported
conformance. Real two-Mount E2E and the Redis lock record are the authority
proof, including conflict, wait, signal interruption, and release semantics.
Upstream source: <https://chromium.googlesource.com/external/git.kernel.org/fs/xfs/xfstests-dev/+/078f3204e049c8ffcf7819bab9afd419267d0084/tests/generic/504>.

The extended E2E run
[`38759450613`](https://github.com/SwordInfra/SwordFS/actions/runs/38759450613)
also confirmed blocking-wait wakeup and signal cancellation. An additional
fixture assertion initially assumed that `fusermount3 -uz` immediately
terminated a daemon while test/child processes retained open descriptors.
This was an invalid assertion: lazy detach does not close a live FUSE
connection while its open file descriptions remain referenced. A subsequent
attempt to send SIGTERM to the foreground daemon was also invalid: the
application's glog failure-signal handler treats SIGTERM as an abnormal exit,
not as a reliable request to run graceful FUSE teardown. The real user-umount
test now calls `fusermount3 -uz` **while the remote waiter is blocked** and
proves that detaching neither grants its lock nor kills the independent
Mount. The waiter is then interrupted through its original syscall, its
remaining file descriptions are closed, and the detached daemon must exit
without hanging or crashing. When a request is in flight during an explicit
lazy detach, libfuse may return `-ECONNABORTED` from its session loop (observed
in CI 38842991060). The E2E fixture accepts that precise exit code only
for a deliberately detached peer, not for unexpected disconnects. This follows
the kernel's lifetime semantics rather than adding
an unexercised, broad Mount cancellation mechanism. Redis errors and ordinary
lock contention never trigger an automatic unmount.

The activation must preserve:

1. A negotiated flock request reaches `SwordFsFlock` and the existing
   Redis `SetFlock`, not a kernel-local-only lock table.
2. Valid Mount owners cannot hold conflicting Redis flock records on the same
   inode. Identity is (volume, Mount generation, opaque kernel lock owner,
   inode, FLOCK); zero is a valid opaque owner.
3. The kernel's shared-open-description lifetime is honored: dup/fork share
   an owner, independent opens do not, `FLUSH` never releases flock,
   `RELEASE(flock_release=1)` releases after the last close.
4. A lost lease or unavailable Redis prevents **new acknowledged grants**;
   it does not automatically unmount FUSE or block unrelated file I/O.
   There is no promise to asynchronously revoke a previously acknowledged
   application-side critical section.
5. `FUSE_CAP_POSIX_LOCKS` remains disabled. OFD byte-range transport
   feasibility is #464/#474, not a prerequisite for BSD flock.
6. The cost of ordinary read/write/metadata operations is unchanged;
   only flock calls and qualifying last-close releases reach lock authority.

## 2. Existing and intended data / call flows

Existing production ownership does **not** need an additional manager:

```text
RunMount
  -> RedisOwnerLease::Register (default for every Redis-backed Mount)
  -> RedisLockAuthority + FuseAdvisoryLocks
       borrows RedisMetaImpl::SharedBlockingExecutor
  -> Mount(FuseAdvisoryLocks *)
       -> MountInitContext { borrowed advisory_locks,
                             posix_locks_negotiated=false,
                             flock_locks_negotiated=false }
       -> VfsHookFactory::SwordFsInit
            requests FLOCK_LOCKS, keeps POSIX_LOCKS disabled
```

After activation, only the capability decision changes the service entry:

```text
app flock(fd, LOCK_SH/LOCK_EX[/LOCK_NB])
  -> Linux FUSE negotiated FLOCK_LOCKS
  -> VfsHookFactory::SwordFsFlock
  -> RunFuseInFiber (SwordFsContext cancellation_token)
  -> FuseAdvisoryLocks::Flock
       -> RedisMetaImpl shared worker -> RedisLockAuthority::SetFlock
       -> EAGAIN? LockRetryScheduler read-only Inspect + original Fiber retry
  -> CanGrant pre-reply gate -> unregister Interrupt -> one FUSE reply

last close -> kernel RELEASE(flock_release, lock_owner)
  -> VfsHookFactory::SwordFsRelease
  -> local handle cleanup, even if it fails
  -> UnlockFlockOwner on shared Redis executor when flock_release=1
  -> one FUSE reply

ordinary FLUSH -> local data flush only (POSIX remote locking disabled)
```

There is **no new lock state, persistent field, Lua script, Redis queue,
dedicated executor, owner translation or compatibility adapter**.
Use existing Folly Fiber/Baton/CancellationToken and SwordFS synchronization
types. The Scheduler's probe is never a grant.

## 3. Capability negotiation and fail-closed startup

**Design-review correction, 2026-10-15:** The former `--enable-lock-owner-lease`
flag was a **temporary #429 staging switch**, introduced when the Redis
owner lease was not yet serving FUSE locks. It is not a useful product-level
choice after #473 makes remote flock operational. **Remove the flag** and
register an Owner Lease by default for every Redis-backed Mount. Keep the
four timing controls configurable; they are tunables, not an enable switch.
SwordFS is Beta and does not need to preserve the obsolete opt-out mode.

- **Kernel supports FLOCK_LOCKS:** always request it using
  `fuse_set_feature_flag(conn, FUSE_CAP_FLOCK_LOCKS)` and record the
  **actual successful negotiation** in `MountInitContext::flock_locks_negotiated`.
  Keep POSIX disabled irrespective of libfuse's default wants. Do not
  retain an alternative kernel-local BSD flock operating mode.
- **Owner Lease registration failed:** still negotiate remote flock if
  supported. `advisory_locks` stays null, so flock calls return
  EIO/unavailable rather than silently succeeding in the kernel-local
  lock table. The ordinary FUSE data/metadata service may remain mounted
  and service requests whenever its own backends remain healthy. A runtime
  lock failure is **never** permission to initiate umount.
- **Kernel lacks FLOCK_LOCKS:** fail mount **initialization** explicitly
  instead of accepting silently local-only flock. Initial mount failure
  differs from an automatic unmount of an already running filesystem.

With only one supported remote BSD flock mode, no separate opt-in fact or
second Boolean is needed in `MountInitContext`. Distinguish the kernel's
capability and actual successful negotiation from whether the Redis owner
service is currently ready; do **not** infer that a non-null adapter is a
necessary condition for capability negotiation. Remove the old flag and
its obsolete config tests/documentation in this same feature PR.

Verify both `want` and `want_ext` consistently via the supported libfuse
feature helper; do not assume callback registration by itself negotiates
FLOCK_LOCKS. The active owner lease may subsequently expire; negotiation
remains fixed, so lock calls continue to fail closed without tearing down
unrelated FUSE service.

## 4. Lifecycle and failure boundary

`SwordFsFlush` must not unlock flock. `SwordFsRelease` always attempts
local resource cleanup; on `flock_release=1`, it independently attempts
the remote unlock (even after local cleanup error), records/logs any
remote failure and uses the existing deterministic error precedence.

User-requested unmount retains the existing `FuseSessionGuard` /
`SwordFsDestroy` / `ShutdownFiberRuntime` / `RunMount` lease-revocation
sequence. With an open file description and a pending `flock` request,
`fusermount3 -uz` detaches the mountpoint without immediately ending the
kernel's FUSE connection. The detached daemon exits after the waiter has
been interrupted and all peer descriptions have closed; a user-requested
lazy detach may yield `-ECONNABORTED` from the libfuse session loop when a
request was in flight. The fixture checks that precise expected exit only
for an explicit detach. SIGTERM is not a
valid graceful-shutdown probe here because the installed glog failure-signal
handler treats it as fatal. These kernel lifetime rules do not justify
Mount-wide cancellation or an unconditional `_Exit(0)`; neither was added.
Never infer a user-initiated unmount from `fuse_session_loop_mt() == 0` alone,
since kernel connection abort can also produce that result. No FUSE exit is
initiated by lease expiry, Redis outage, lock conflicts, or cancellation.

The already accepted #430 Beta failure limit remains: Redis EXEC outcome and
FUSE reply are not atomic. Unknown grant results use bounded retry then
**error only**, not a global reply journal or blind compensating unlock.

## 5. Tests and integration proof (test-first)

The integration proof uses **two independent kernel FUSE mounts**
of the **same formatted Redis volume**, each with a distinct registered
Mount generation, plus a third test process where helpful. Two
`MountInitContext` instances constructed inside one C++ unit test do not
constitute this proof.

| Case | Required observable evidence |
| --- | --- |
| Negotiation | FLOCK_LOCKS requested and accepted in both real mounts; POSIX_LOCKS stays off; live `SwordFsFlock` callbacks and Redis-backed owner records are observed |
| Nonblocking | SH/SH succeeds across mounts; cross-mount SH vs EX and EX vs EX fail with EWOULDBLOCK, including distinct independent opens; one mount's kernel-local lock table cannot explain the result |
| Blocking | A second mount waits on the same inode until remote unlock, then succeeds; cancellation of the waiting syscall returns EINTR without post-EINTR new grants |
| Description lifetime | dup/fork share flock ownership; independent opens conflict; intermediate close / FLUSH retains lock, final RELEASE(flock_release=1) releases it |
| Conversion | Same-owner SH to EX upgrade/downgrade and two-owner competing upgrades conform to existing #430/#471 semantics |
| Failure isolation | Lease expiry/Redis failure prevents new locks but leaves ordinary `stat`/read/write mounted and available wherever metadata service remains healthy |
| User umount | Clean user-driven unmount and lease revocation; evaluate at least a pending-waiter boundary, with no hang, crash, leak or unrequested automatic unmount |
| Unsupported capability | A Mount on an unsupported kernel fails initialization rather than silently accepting kernel-local flock; no opt-out/local-only mode remains |
| Default behavior | A Mount without any lock-specific enable flag registers an Owner Lease and negotiates remote BSD flock; no such flag is required in either Mount process |

Extend `tests/e2e/Fixture` only as needed to supervise a **second
foreground SwordFS process** and mountpoint with the **same volume**;
do not format a second volume or duplicate the complete fixture. Use
scoped cleanup and semantic completion barriers (lock-owner activity,
syscall result, confirmed final close), not fixed sleeps as admission
proof. Include a real end-to-end test in GitHub CI, not just unit-level
FUSE hook mocks.

**TDD:** The new real kernel-to-Redis two-Mount tests must first demonstrate
a meaningful RED condition under the currently disabled remote capability,
then the smallest change in `SwordFsInit` / `MountInitContext` and
needed test harness produces GREEN. GitHub CI is the compilation/test
environment; no local SwordFS builds. Final PR runs the full matrix,
tests affected production C++ file patch coverage >90%, independent review,
and post-merge main verification.

## 6. fstests classification and rollout

Before activation, `generic/504` recorded `kernel_local_only/PASS` under #433,
which was **not** distributed flock support. After default remote flock
negotiation, the pinned upstream test fails its `/proc/locks` lookup because
the authoritative flock record now lives in Redis, not in the local kernel
lock table. The exact observed `lock info not found` output is recorded in
`conformance/fstests/fail-evidence/generic/504.out.bad`, and the upstream
test is classified as `upstream_not_applicable` in `known-gaps.tsv`, **not** as
supported. The first GREEN run's 645 observed fstests outcomes were replayed
against that baseline with zero blockers, and subsequent formal CI passed.
Actual two-Mount E2E tests establish distributed lock semantics independently
of this incompatible upstream introspection requirement. No lease opt-in flag
is passed to the mount fixture; remote flock is the default.

Leave traditional POSIX `generic/131` and mixed OFD/POSIX
`generic/478` as kernel-local-only, pending #464/#474.

## 7. Peer-source comparison and explicit non-goals

[JuiceFS's FUSE frontend](https://github.com/juicedata/juicefs/blob/main/pkg/fuse/fuse.go)
uses `EnableLocks` and forwards flock to its VFS/meta layer with
request cancellation. Its Redis metadata backend independently represents
BSD flock and POSIX ranges. SwordFS already has equivalent semantic
entrypoints and does **not** need to copy JuiceFS's extra local
handle-level lock-owner mixing or session reverse index: #428 verified
the opaque flock owner on final RELEASE for the pinned kernel/libfuse.

libfuse's documented `fuse_set_feature_flag` checks kernel support and
changes `want_ext`; handler registration alone is not evidence of a
kernel-negotiated feature.

Out of scope: OFD record locks, activating FUSE_CAP_POSIX_LOCKS, global
deadlock graphs, per-process sessions, automatic umount, async force-release
of already acknowledged locks, persistent lock wait records, custom schedulers
or Redis Pub/Sub, a new thread pool and a 10K-lock scalability rewrite.

## 8. Design-review decision

**Architecture implemented; independent review pending.** Test-first RED
established the old local-only behavior. Activation of `FUSE_CAP_FLOCK_LOCKS`
and default owner lease registration made the cross-Mount contract GREEN,
including fail-closed startup, lock-only lease failure, wait interruption,
last-close release, and lazy-detach lifecycle. The final PR/merge gates still
require independent review and post-merge main CI verification.
