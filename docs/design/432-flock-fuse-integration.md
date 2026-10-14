# #432 — BSD flock FUSE integration and final-close release

**State:** Nonblocking BSD flock adapter, FUSE hook and final-RELEASE cleanup
implemented. Remote flock is **not activated**. #433 owns blocking waits,
interruption, FUSE
capability activation and authoritative fstests promotion; #464 owns OFD
**byte-range** locks (not BSD flock).

Parent #379; prerequisites #428 and merged #429/#430/#431. Shared design:
[POSIX conformance](posix-conformance.md); peer design:
[JuiceFS VFS](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs_unix.go).

## 1. Existing system and proven transport behavior

- RedisLockAuthority::SetFlock already supports whole-inode shared/exclusive/
  unlock, WATCH + authoritative Owner Lease validity, bounded ambiguous EXEC
  retries, per-inode stale-owner cleanup, and a **separate** flock namespace.
- Previously VfsImpl::FLock returned NotSupported (ENOSYS); #432 routes the
  FUSE callback directly to the shared advisory-lock adapter and removes the
  now-unused VfsImpl placeholder. RELEASE snapshots the fh, opaque flock
  owner and flock_release before fiber deferral.
- The #431 PosixRecordLocks adapter establishes the correct fiber-to-thread
  boundary via the existing BlockingExecutor. The Mount owns its lease,
  Redis authority, executor and adapter; MountInitContext borrows them.
- #428's 26/26 Linux 6.17/libfuse 3.18 observer proves the kernel-supplied
  flock lock_owner survives dup/fork sharing one open-file description.
  Intermediate close causes FLUSH, not RELEASE. Final RELEASE carries
  flock_release=1 **and the same flock owner**. FLUSH uses a different POSIX
  process owner and must never be used to release flock.
- libfuse states that flock_release is meaningful only for RELEASE and a
  true flag guarantees a valid lock_owner. Because libfuse may enable remote
  flock automatically when a handler exists, explicitly disable
  FUSE_CAP_FLOCK_LOCKS until #433.

### JuiceFS peer-source comparison (reviewed 2026-10-13)

The reference implementation exposes one metadata lock service with
separate semantic operations:

- [JuiceFS VFS Flock/Getlk/Setlk](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs_unix.go)
  offers separate semantic entry points under one VFS and metadata service;
  [Meta interface](https://github.com/juicedata/juicefs/blob/main/pkg/meta/interface.go)
  similarly has Flock, Getlk and Setlk rather than independent engines.
- [Redis Flock and POSIX methods](https://github.com/juicedata/juicefs/blob/main/pkg/meta/redis_lock.go)
  share transaction machinery but use flockKey/plockKey Hashes. Flock keeps
  one R/W state per owner, while POSIX stores ranges including PID. The
  two classes do not conflict on Linux.
- [JuiceFS Release and Flush](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs.go)
  distinguish last-close from process-close cleanup. JuiceFS also tracks
  local handle locks/flockOwner and mixes flock metadata owner with fh
  (owner XOR fh), in part for older kernels lacking owner in RELEASE.
  **Do not copy those fields or owner mixing without evidence:** #428
  verified this target FUSE delivers the opaque flock lock_owner with
  flock_release on final RELEASE; #430 already qualifies it using the
  unique Mount generation.
- JuiceFS Redis maintains a session locked-key reverse set for cleanup.
  SwordFS's reviewed #429/#430 architecture instead uses a mount lease
  and bounded per-inode lazy GC. Do not reintroduce a reverse index.

**Decision:** one distributed authority, two semantic lock classes, and
one FUSE fiber/thread adapter object. No second flock lock manager.

## 2. Identity and state contract

    flock identity = (volume, mount_generation, kernel_lock_owner, inode, FLOCK)

Use fi->lock_owner **unchanged**, including zero. Never derive ownership
from PID, local fh, a process table, an OFD byte-range owner or a POSIX
record-lock owner. The kernel supplies the same owner for inherited dup/fork
descriptors; independent open file descriptions have separate owners.

SH/SH is compatible; other-owner EX conflicts with SH or EX. A same-owner
upgrade/downgrade replaces that owner's single canonical flock state. An
explicit LOCK_UN is a full-owner flock release. POSIX byte-range locks
occupy an independent Redis Hash and do not conflict with flock on Linux.
No per-fd record index, new session, owner-to-inode reverse map, additional
lease or Redis lock engine is needed.

## 3. FUSE, executor and lifetime

Reuse **one** fiber/thread adapter object instead of constructing a second
flock-specific adapter. #432 renamed #431's narrowly named PosixRecordLocks
to FuseAdvisoryLocks (no Beta compatibility alias).
Keep separately named POSIX Set/Get/Unlock and flock Set/Unlock methods
within that adapter: operation flags, ranges, PID and close semantics
remain different. One adapter borrows one ILockAuthority and one
BlockingExecutor. Do not invent a generic range-as-flock conversion,
variant-heavy request DSL, additional thread pool or new Redis client.
Construction and teardown follow #431.

    FUSE flock callback
      -> snapshot (inode, fi->lock_owner, op) before fiber deferral
      -> capability and op validation
      -> FuseAdvisoryLocks::Flock in fiber
      -> BlockingExecutor::RunFromFiber
      -> ILockAuthority::SetFlock on POSIX worker thread
      -> RedisLockAuthority WATCH / PTTL / EXEC
      -> check CanGrant() for successful SH/EX just before reply
      -> one fuse_reply_err

    FUSE RELEASE callback
      -> snapshot (inode, fi->fh, fi->flock_release, fi->lock_owner)
      -> VfsImpl::Release always attempts local resource teardown
      -> when remote flock is negotiated AND flock_release:
           FuseAdvisoryLocks::UnlockFlockOwner
              -> BlockingExecutor -> SetFlock(kUnlock)
      -> log both outcomes, choose deterministic error precedence
      -> one fuse_reply_err

    FUSE FLUSH callback
      -> #431 traditional process-lock cleanup only
      -> NEVER BSD flock cleanup

RELEASE must always try the remote unlock after a delivered flock_release,
even when the local handle is invalid or its final flush fails. Conversely,
Redis errors cannot prevent local FileHandle::Release from unregistering
the local handle. Prefer retaining the local-release error if both duties
fail, and log the remote error. A FUSE RELEASE reply is not a reliable
application-visible close error, so failures must be diagnosed honestly.

Only perform remote flock work when its own negotiated capability is true.
Rename MountInitContext's current posix_locks borrowed pointer to
advisory_locks; do not add a second adapter pointer. Retain the distinct
posix_locks_negotiated boolean and add flock_locks_negotiated. Never
infer flock capability from POSIX negotiation or lease registration.
When disabled, preserve existing ENOSYS and **zero new Redis calls** on
ordinary FLUSH or RELEASE. The existing #431 POSIX callbacks and tests
must remain correct after this one-time Beta adapter rename.

The session destroy hook drains all admitted fibers before RunMount revokes
the generation and destroys the single advisory-lock adapter, shared executor,
authority and lease. Never call Redis or ThreadMutex on a FUSE fiber.
Never hold a local file-handle lock while awaiting a remote unlock.

## 4. Operation validation, failure policy and costs

- SH/EX with LOCK_NB: call the #430 nonblocking authority. A conflict
  returns EAGAIN (= EWOULDBLOCK on Linux), capacity ENOLCK, unavailable
  Owner Lease or Redis EIO. Maintain a single FUSE reply.
- LOCK_UN, optionally combined with LOCK_NB: full-owner unlock; repeated
  unlock is a safe no-op through #430's existing transient transaction fence.
- Unknown/mutually conflicting flag combinations: EINVAL without a grant.
- Blocking SH/EX **without** LOCK_NB: EOPNOTSUPP, never silently convert
  a blocking call into an immediate-only trylock. #433 owns waiting.
- Before successful SH/EX reply, recheck the shared #429 CanGrant local
  cutoff. The final check-to-send race cannot be eliminated and is not
  a reason to introduce unrelated I/O fencing.
- A lost/unknown Redis EXEC uses #430's bounded same-intent replay, then
  **returns error only** on exhaustion. Do not revoke Mount generation,
  unmount FUSE, create a result journal, or blindly rollback successful
  EX->SH conversion if the FUSE reply failed.

Failure boundary: an unacknowledged grant can remain while its Mount lease
stays valid, and a last-close Redis failure can likewise leave a remote
field. The kernel may not issue flock_release after an unsuccessful lock
call. This is the already-accepted #430 Beta error-only limitation, **not**
a promise of guaranteed per-OFD cleanup under Redis failure. #433's
feature-advertising review must account for the limitation; do not hide it
with a per-fd owner index or background retry daemon.

Costs: one authority transaction per explicit nonblocking flock operation
and one independent unlock transaction per qualifying final RELEASE when
negotiated. No mount-wide scan and no additional Redis operations on reads,
writes, truncate, intermediate FLUSH or ordinary release without flock.

## 5. Feature activation and fstests

In SwordFsInit, explicitly unset **FUSE_CAP_FLOCK_LOCKS** even if libfuse
prepopulated its requested flags. #431 already explicitly disables
FUSE_CAP_POSIX_LOCKS. Merely implementing FUSE callbacks must not silently
expose incomplete blocking semantics. #433 is the activation owner.

Actual kernel-local flock success without remote capability is not evidence
of distributed locking. On #432 full CI (Linux 6.17/libfuse 3.18),
`generic/504` passed with remote `FUSE_CAP_FLOCK_LOCKS` explicitly disabled:
its `/proc/locks` observation therefore establishes only kernel-local
flock. Its previous `known_unsupported` / `FAIL` #255 baseline would
incorrectly flag this as `XPASS`. Classify this observed result as
`kernel_local_only` / `PASS` under #433, matching the existing treatment
of `generic/131` and `generic/478`. Keep `generic/504` out of
`supported.txt` and retain strict regression detection if it begins to
fail or stop running. An actual kernel-mounted, negotiated, two-Mount and
blocking conformance gate remains #433. OFD byte-range transport #464
is a separate concern.

## 6. C++ TDD/verification contract

Use real Redis + real FiberManager + existing FUSE reply capture. The
test-first RED compile failure was confirmed on the original test-only
revision, followed by focused CI GREEN (5/5 Redis/FUSE adapter tests) on the
first implementation revision. Full PR CI and Codecov are separate gates:

| Case | Expected externally observable result |
| --- | --- |
| SH + SH across mounts | Both granted, even if raw owner numbers coincide |
| SH/EX, EX/SH, EX/EX + LOCK_NB | EWOULDBLOCK/EAGAIN; no conflicting grants |
| Same-owner SH <-> EX conversion | Atomic state replacement, correct conflict checks |
| LOCK_UN and repeated LOCK_UN | Only exact owner released; safe no-op |
| POSIX record lock and flock | Independent conflict classes |
| Invalid flags, blocking without NB | EINVAL / EOPNOTSUPP, no unintended grant |
| dup/fork vs independent opens | Shared vs distinct owner, per #428 transport evidence |
| Shared adapter refactor | All existing POSIX SETLK/GETLK/FLUSH tests remain GREEN; one adapter/authority/executor |
| Intermediate FLUSH and final RELEASE | No flock release on FLUSH; release only at final |
| Invalid fh / local Release error | Remote unlock still tried and local resources freed |
| Redis failure during RELEASE | Local resources freed, error/log visible, no false success |
| Expired Mount generation | No new successful grant or acknowledged renewal |
| Unknown EXEC or failed FUSE reply | Bounded error-only retry; never blind conversion rollback |
| Unnegotiated capability and no lease | FLOCK flag off; no new release-time Redis RPC |
| Unmount with active flock | Fibers drained then exact-generation lease revoked |

Do not use arbitrary fixed sleeps, internal redis-field ordering or private
callback order as semantic assertions. Real-fuse dup/fork and cross-mount
activation remain #433; #432 adapter tests may force a negotiated mock
context to test its implementation without claiming real activation.

**Design disposition:** The bounded nonblocking #432 slice is implemented;
FUSE remote flock must NOT be advertised. #433 remains the blocking,
negotiated integration and conformance gate.
