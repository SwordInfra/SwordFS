# #431 — Traditional POSIX record-lock FUSE integration

**Scope/state:** implementation of *traditional process-associated* `fcntl`
`F_GETLK`/nonblocking `F_SETLK`/`F_UNLCK` and per-close owner release. Redis
authority #430 and mount-generation lease #429 are already implemented. No
runtime FUSE POSIX lock capability is enabled in #431. #433 owns blocking
`F_SETLKW`, interruptions, actual feature activation and fstests promotion.
**OFD record-lock implementation and its unresolved FUSE transport/lifetime
problem belong exclusively to #464**; BSD flock belongs to #432.

## 1. Evidence and inherited constraints

- #428's 26/26 Linux 6.17 / libfuse 3.18.2 observer tests establish that a
  process-associated owner arrives as opaque `fi->lock_owner` in `getlk`,
  `setlk` and `flush`; the value is not a PID or a FileHandle. Two mounts
  require distinct #429 generations even if the opaque owner values match.
- On normal `close()`, FUSE `FLUSH` carries the process lock owner even for a
  different `open()` of the same inode or an intermediate `dup()` close.
  `RELEASE` is a different *last-open-reference* event: it cannot replace
  traditional POSIX close cleanup.
- Linux can omit delivery of FUSE `FLUSH` for particular prior writeback/
  mapping failures or `FOPEN_NOFLUSH` paths. This is confirmed in
  `fs/fuse/file.c` and explicitly documented as an exception by libfuse.
- #430's `ILockAuthority::SetPosix/GetPosix` already provides canonical
  nonblocking READ/WRITE/UNLOCK byte-range states; `RedisLockAuthority`
  runs **only in the thread execution domain**. FUSE's callbacks dispatch
  into SwordFS's **fiber domain**. The existing
  `utils::BlockingExecutor::RunFromFiber()` is the required offload bridge.
- #429 lease registration is currently opt-in, and a failed registration
  cannot justify local-process fallback or abort ordinary filesystem I/O.
- JuiceFS' low-level FUSE path passes the kernel lock owner through to its
  VFS/meta `Getlk/Setlk`, and passes the `FLUSH` owner to its VFS. Its
  published POSIX compatibility contract supports **traditional** record
  locks, not OFD record locks. Adopt the semantic pattern; do not import a
  global Session service or assume JuiceFS proves OFD isolation.

References: [Linux `fs/fuse/file.c`](https://github.com/torvalds/linux/blob/master/fs/fuse/file.c),
[libfuse low-level API](https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html),
[JuiceFS VFS](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs.go),
[JuiceFS POSIX compatibility](https://juicefs.com/docs/community/posix_compatibility/),
[#428 evidence](posix-conformance.md), [#430 design](430-redis-lock-authority.md).

## 2. Owner, data model and contract

```
record identity:  (volume, mount_generation, kernel_lock_owner, inode)
lock content:    normalized READ / WRITE intervals for that record identity
GETLK PID:       request-supplied holder PID in the existing POSIX range
                 record, NOT an owner identity or a per-process index

FUSE request callback (fiber) -> semantic POSIX lock adapter
    -> BlockingExecutor::RunFromFiber -> ILockAuthority (thread)
    -> RedisLockAuthority WATCH/lease/record Hash/EXEC
```

Use `fi->lock_owner` exactly as received, including zero if valid for the
kernel transport. Do not derive Owner from `fuse_req_ctx(req)->pid`, `fh`,
filesystem UID or the client TCP address. The backend already adds its own
Mount generation to that owner; do not add a second Session or another local
lock authority. `GETLK` ignores locks held by that *same* generation/owner.

`struct flock` is delivered by libfuse in a **normalized `SEEK_SET` form**.
The current libfuse converts the FUSE inclusive end into nonnegative `l_len`,
with `l_len == 0` for open-ended EOF. Validate `l_type`, `l_whence`,
nonnegative `l_start`/`l_len`, signed-to-unsigned boundaries and overflow
before calling #430. Do not re-resolve `SEEK_CUR`/`SEEK_END` against a
possibly-changing inode size: the kernel/libfuse path owns normalization.
The #430 `PosixRange{start,length,type,pid}` is sufficient for ordinary ranges
and for full-owner close-unlock `{0,0,kUnlock}`.

`GETLK` must call #430 `GetPosix`, return `F_UNLCK` when unblocked, or
construct a complete `struct flock` with the conflicting range/type and
`l_whence = SEEK_SET`; reply using **`fuse_reply_lock()`**, never a successful
`fuse_reply_err(req, 0)`. #430's `PosixConflict` has only `kernel_owner`,
which **cannot be cast to the holder PID**. This PID is **required** for a
conflicting *traditional POSIX* record lock by `fcntl(F_GETLK)` semantics,
not an optional SwordFS diagnostic enhancement. JuiceFS independently
passes `pid` to metadata `Setlk/Getlk` and stores it directly in each
`plockRecord` alongside type/start/end. Follow the same minimal principle:
extend #430's POSIX range record/`PosixConflict` with the **PID supplied by
the kernel's original SETLK request**, returning the PID associated with
the conflicting segment. Store no extra PID index, process table, liveness
state, or other Redis lookup; PID **never participates in lock authority**.
The value is host-/PID-namespace-local and can become stale; do not invent
global cross-host PID identity or substitute `kernel_owner`. Validate
signed/unsigned conversions and the exact FUSE PID delivery/GETLK behavior
through normal #431 tests. The Redis owner value uses `P2|type,start,end,pid`
segments; adjacent segments coalesce only when both type and diagnostic PID
match. Since SwordFS is Beta, there is no legacy `P1` record decoder.
OFD's distinct `l_pid = -1` case is out of scope
(#464), not a replacement for the traditional POSIX field.

Read locks are compatible with other read locks; any overlapping other-owner
write lock conflicts, as does write vs either other-owner lock. An owner
may replace, split, merge and partially unlock its own ranges. Preserve
the existing #430 error behavior: conflict -> `EAGAIN`/`EACCES` as agreed
with Linux userspace contract, resource limit -> `ENOLCK`, Redis/lease
fault -> reported failure, never a local-lock-only success.

## 3. FLUSH, reply ordering and failure contract

`SwordFsFlush(req, inode, fi)` must snapshot **both `fi->fh` and
`fi->lock_owner`** before submitting a fiber. Once a Mount has actually
negotiated the remote POSIX lock capability (#433), a delivered `FLUSH`
has two separate duties:

1. Attempt normal FileHandle/data flush using existing `VfsImpl::Flush`.
2. **Regardless of data flush success**, attempt #430 whole-owner
   `SetPosix(inode, lock_owner, {0,0,kUnlock})` on the lock executor.

These are independent duties; a writeback error must not cause an early
return that prevents the unlock. An invalid/unavailable `fh` also cannot by
itself suppress the attempted owner unlock: `inode + lock_owner` is
sufficient lock identity. Return an error if either required duty failed,
with deterministic precedence when both fail and a diagnostic log for the
other error. Reply once, only after both duties have completed. Do not
release flock on FLUSH: #432 owns flock's last `RELEASE`.

For correctness and simplicity initially perform the explicit Redis unlock
on **every delivered FLUSH while remote POSIX locking is active**, even if
the Mount has no remembered lock on
that inode; a no-op unlock is safe through #430's transient HSET/HDEL
fence, which also protects against delayed uncertain EXEC. This costs a
Redis transaction per qualifying close; record the latency/RTT overhead in
the performance review before considering an optimization. A client-only
"probably no lock" hint must never cause skipped cleanup after an
unconfirmed grant.

If the kernel **does not deliver a FLUSH**, the daemon has no trustworthy
individual-process close event. An expired *Mount* lease only makes that
entire Mount's records inert; it does not clean one exited process while
its Mount remains healthy. **Explicit Beta limitation:** no unconditional
per-process close invalidation can be promised in this exceptional case.
Do not introduce PID polling, speculative /proc checks, per-process leases,
generic Session state or a mount-global scan to pretend it is solved. This
limitation must be stated in tests/docs and reconsidered as part of #433's
feature-advertising decision; if an observed normal-close loss violates
the proposed supported contract, the capability stays disabled.

#430's `OutcomeUnknown` policy is **bounded same-intent retry, then error
only**, with no forced FUSE unmount, lease revoke or durable Journal. A
failed `SETLK` acknowledgement may still have committed; neither a failed
FUSE reply nor a lock conversion justifies blind rollback (e.g. EX -> SH
followed by another reader joining). After successful redis SETLK, recheck
#429's `CanGrant(generation)` immediately before sending the one success
reply, acknowledging the unavoidable final check-to-send race. #433 owns
interrupt/cancel and any later, proven conditional compensation policy.

## 4. Runtime bridge, lifetime and capability boundary

Create/bind the per-Mount Redis lock authority and bounded lock executor
only with the existing #429 registered generation; if the lease is absent,
lock operations fail closed while ordinary read/write and FUSE remain
available. Expose it to low-level callbacks using the existing Mount/FUSE
request context (rather than an unrelated singleton), preserving lifetime:

1. Initialize owner lease / authority / executor on the Mount control
   thread, and keep the lease alive longer than authority.
2. Capture `fi`, `struct flock`, `inode` and identity by value before
   `RunFuseInFiber`; suspend fibers via `RunFromFiber`, never invoke a
   blocking Redis client or a `utils::ThreadMutex` on the fiber thread.
3. During teardown follow existing FUSE loop completion and thread-executor
   resource lifetimes: no worker may access a destroyed authority/lease;
   complete or safely stop outstanding work before releasing its objects,
   and revoke the exact generation Lease following #429's normal-umount
   contract. This is **memory/resource safety**, not a new cross-request
   serialization guarantee. No per-inode lock enumeration, mount-global
   operation barrier beyond safe teardown, or indefinite shutdown wait.

In the implementation, `RunMount` constructs the authority, two-thread
`BlockingExecutor`, and `PosixRecordLocks` adapter only after successful
Owner Lease registration. The generic adapter depends solely on
`ILockAuthority`, which exposes a nonblocking, fiber-safe `CanGrant()` gate;
the Redis implementation owns its concrete Owner Lease dependency. The
adapter is borrowed by FUSE's existing
`MountInitContext` userdata. The FUSE session's destroy hook drains admitted
fibers before `RunMount` revokes the lease and destroys the adapter, executor,
authority and lease in that order. No process-table state or global lock
scheduler is added. `SwordFsInit` explicitly clears `FUSE_CAP_POSIX_LOCKS`;
the `posix_locks_negotiated` guard prevents all Redis close activity until
the activation work in #433.

**SETLK versus FLUSH concurrency:** POSIX requires an already successful
grant to be released by a later close on the same file, and Redis operations
for the same `(inode, generation, owner)` must not overwrite each other's
updates. #430 already serializes same-Owner mutations and WATCHes the
authoritative Hash. Preserve these existing semantic constraints. If SETLK
and close/FLUSH **overlap without a defined happens-before relationship**,
do not add a new FIFO dispatcher, sequence counter, waiting fence, or global
ordering guarantee: either valid operation order is permissible subject to
the ordinary close/lock semantics. Tests should cover required completed-
before-close behavior and legitimate overlapping outcomes, without assuming
a specific scheduler/interleaving order.

While #431 keeps remote POSIX capability **disabled**, `FLUSH` must preserve
existing data-flush behavior and not attempt remote unlock or fail ordinary
`close()` merely because a separate #429 lease was not registered. The
lock-release branch must be explicitly tied to the *negotiated* remote-lock
service, not merely the presence of an authority instance.

`#431` itself **must not set** `FUSE_CAP_POSIX_LOCKS`, activate blocking
SETLKW, silently fall back to kernel-local locks, or promote conformance.
For a Mount that has not negotiated that capability, preserve the existing
low-level FUSE `GETLK`/`SETLK` response of `ENOSYS`, rather than returning
`EOPNOTSUPP`: changing the unsupported-response contract caused
`generic/131` and `generic/478` to become XPASS in #431's fstests run even
though no cross-Mount authoritative feature was enabled. Restoring `ENOSYS`
in the unnegotiated callbacks was necessary to preserve the existing low-level
contract, but **did not remove either XPASS**: the kernel can satisfy both
upstream tests locally without calling those callbacks. The strict XPASS
gate correctly rejected the original FAIL expectations. #431 now classifies
only these two testcases as `kernel_local_only` with an explicit expected
`PASS`, without adding them to `supported.txt`. Their actual PASS remains
mandatory, and any FAIL/NOTRUN, missing result, core dump or isolation failure
remains blocking. This is not distributed POSIX/OFD conformance evidence;
#433 must supply independent negotiated, cross-Mount FUSE evidence before
any authoritative promotion or activation. Once the
capability is negotiated, an unavailable Redis lock service must fail
closed (`EIO`) instead of returning `ENOSYS` or pretending a lock succeeded.
The #428 probe demonstrated that without remote POSIX capability the
kernel generally performs its *own mount-local locking* without calling
the remote FUSE lock callback. Therefore #431's Redis-backed TDD should
exercise the adapter/authority seam and isolated multi-Mount service
instances, not pretend an ordinary unadvertised `fcntl()` invocation
performed a remote lock. #433 owns the first authoritative full-FUSE
cross-Mount validation with negotiated capabilities.

**#464 interaction:** OFD `F_OFD_SETLK` arrives through the same FUSE
SETLK entry point with no reliable OFD flag or last-close owner in the
target kernel evidence. Excluding OFD *implementation* from #431 cannot
magically reject it after #433 enables POSIX remote locks. #433 **must**
decide the honest supported/unsupported and safe-activation policy based
on #464 evidence; this is an integration safety gate, not a requirement to
implement OFD in #431.

## 5. TDD and acceptance matrix

**RED (before C++ implementation):** unit/adapter/real Redis tests must
demonstrate the missing semantics, in the narrow focused CI where possible.

| Area | Required observable scenario |
| --- | --- |
| Identity | Two separate `open()` calls of one process share lock_owner; different process and separate Mount generations conflict appropriately; `fh`/PID are never the lock identity |
| Range / conflict | nonblocking READ/READ allowed; READ/WRITE and WRITE/WRITE conflict; split/merge, conversion, partial unlock and EOF; malformed input and `ENOLCK` |
| GETLK | no conflict `F_UNLCK`; conflicting start/length/type and **actual kernel-supplied holder PID stored on the range**; self-lock excluded; no unverified result after Redis fault; `fuse_reply_lock` mapping |
| Normal close | close unrelated FD, intermediate `dup` close, fork/exec inherited FD events, and final close: delivered `FLUSH` unlocks matching POSIX owner without using `fh` as identity |
| FLUSH failure | data flush fails or `fh` lookup fails but delivered `FLUSH` still attempts Redis unlock; unlock failure propagates/logs while release of other resources proceeds |
| Async/race | capture caller-owned FUSE arguments; lock worker runs in thread-domain; completed SETLK before delivered close unlocks normally, overlapping close/SETLK has no invented FIFO ordering; no deadlock or lost completion; teardown prevents use-after-free |
| Faults | Redis lease expired or absent; unknown EXEC applied/not-applied and bounded error-only exhaustion; failed reply after grant/conversion does not blindly revert other-owner state |
| Future activation | #433 verifies actual FUSE two-Mount `fcntl` with negotiated capability and `generic/131` prior to `supported.txt` promotion, plus #464 OFD transport-safety decision |

**Implementation disposition:** #431 entered test-first implementation after
a real Redis RED in Dev Build run 38360641213: the prior GETLK returned PID
`0` for holders with supplied PIDs `1234` and `5678` (compilation succeeded).
`GETLK.l_pid` is an existing POSIX-required field and is
persisted minimally in the existing range state, while SETLK/FLUSH ordering
requires only ordinary POSIX completed-before-close semantics and safe
same-Owner mutation/resource lifetimes, **no new stronger concurrency
protocol**. Exceptional missing-FLUSH remains an explicitly documented
Beta limitation; #433 must review it and the #464 transport issue before
feature activation. Exact conversion, reply and shutdown cases are TDD
acceptance tests, not reasons to introduce a general process/session
service or OFD feature into this task. Implementation uses the existing #430
authority and thread bridge; no conformance labels or mount capability are
promoted. RED/GREEN evidence, CI results and review outcomes are tracked in
Issue #431 and PR #465 rather than inferred from this document.
