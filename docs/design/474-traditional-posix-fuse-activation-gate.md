# #474 — Activate traditional POSIX record locks: JuiceFS peer review

**Updated design decision (2026-10-16):** retract the earlier blanket
“POSIX activation blocked until reliable OFD rejection” conclusion.
The kernel still does **not** provide a reliable POSIX-versus-OFD request
class discriminator. However, **that absence alone is not a valid reason
to disable traditional POSIX record locks altogether**: JuiceFS forwards
them through the same Linux FUSE interface today. The supported-product
contract and unsupported-OFD limitations must be assessed separately.

**Scope and verification checkpoint (2026-10-19, before merge):** #474's PR #494 activates *traditional*
process-associated `F_GETLK`, `F_SETLK` and `F_SETLKW` through the existing
SwordFS Redis lock authority. The real two-Mount tests and full CI passed
at review head `df6fc74` ([run 39216797074](https://github.com/SwordInfra/SwordFS/actions/runs/39216797074):
22 successful jobs); both changed production files exceeded 90% patch coverage.
At this checkpoint, the PR was **Ready for Review** and had not yet merged.
This dated record is verification history, not a live statement about `main`.
**OFD record locks are NOT SUPPORTED / NOT PLANNED** (#464 closed);
no OFD functionality or separate OFD feature task is proposed.
The existing shipped Redis-backed BSD `flock` remains unchanged.

## 1. What JuiceFS actually does (code-backed)

Code reviewed from [juicedata/juicefs](https://github.com/juicedata/juicefs)
at main revision `506808b0c521` (2026-10-16):

1. [pkg/fuse/fuse.go](https://github.com/juicedata/juicefs/blob/main/pkg/fuse/fuse.go)
   sets `opt.EnableLocks = true`. Its low-level `GetLk`,
   `SetLk`, and `SetLkw` callbacks forward `in.Owner`, file handle,
   range/type/PID and wait mode into VFS. `FUSE_LK_FLOCK` is checked
   solely to dispatch BSD flock separately. It does **not** identify or
   reject OFD record-lock requests.
2. [pkg/vfs/vfs_unix.go](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs_unix.go)
   calls `v.Meta.Getlk` / `v.Meta.Setlk` for the supplied opaque owner.
   After a successful non-unlock `Setlk`, the handle sets
   `h.locks |= 2` and remembers the **first** `owner` in a field named
   `h.ofdOwner`. This stores an owner; it does **not** establish that
   the kernel request was actually an OFD-class request.
3. [pkg/vfs/vfs.go](https://github.com/juicedata/juicefs/blob/main/pkg/vfs/vfs.go)
   `Flush` attempts `Meta.Setlk(... lockOwner, F_UNLCK,
   full-range ...)` on each delivered close while the handle has had a
   record lock. It clears the remembered handle owner when a matching
   `FLUSH` is delivered. On final `Release`, if that remembered owner
   is still nonzero, it also attempts full-range `Meta.Setlk(...,
   powner, F_UNLCK ...)`. This is a bounded **best-effort fallback
   cleanup**, not guaranteed class discrimination or full OFD semantics.
4. [JuiceFS compatibility documentation](https://juicefs.com/docs/community/posix_compatibility/)
   openly advertises **traditional** POSIX record locks and explicitly
   excludes OFD record locks due to Linux FUSE kernel limitations.

Thus JuiceFS does **not** solve the untagged FUSE request-class problem.
Instead it ships the useful traditional POSIX lock contract while treating
OFD as unsupported, with practical per-handle owner cleanup to reduce stale
records. A single remembered owner cannot in general represent arbitrary
multiple process/open-description owners reusing one FUSE handle, nor prove
correct semantics in every exceptional FLUSH scenario. Do not misrepresent
this as reliable OFD support or proof that untagged OFD can be rejected.

## 2. Linux FUSE transport facts that remain true

Completed [#428](https://github.com/SwordInfra/SwordFS/issues/428)
tested Linux `6.17.0-1022-azure` / libfuse `3.18.2`,
26/26 observer assertions (CI
[#37791689635](https://github.com/SwordInfra/SwordFS/actions/runs/37791689635)).
The untagged low-level `GETLK/SETLK` request can carry *both*
traditional and unsupported OFD record operations; the only exposed class
bit `FUSE_LK_FLOCK` distinguishes BSD flock. OFD's opaque owner is not
the traditional process owner delivered in `FLUSH`, and final
`RELEASE` does not identify that OFD owner.

Sources: Linux
[fs/locks.c](https://code.googlesource.com/linux/torvalds/linux/+/f155f3b3ed1af23884ffaffe8a669722b87ac9d6/fs/locks.c),
[fs/fuse/file.c](https://code.googlesource.com/linux/torvalds/linux/+/23eb7901811a06805ec7d6e766dd82f083d03204/fs/fuse/file.c)
and [FUSE wire ABI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/fuse.h).

**Do not attempt to classify request kind from PID, `fh`, `l_pid`
or a guessed opaque owner value.** That remains unprovable. In particular,
an unsupported OFD syscall may receive a misleading success response
if forwarded untagged; correct OFD behavior is **not** advertised. Its
possible interference with supported lock users must be characterized
and documented, not quietly called safe or solved.

## 3. Revised #474 activation contract

The previous design made *perfect filtering of unsupported OFD calls*
a prerequisite to supplying traditional POSIX locks, effectively making
the supported feature impossible despite the established JuiceFS pattern.
**Remove that extra blanket gate.** Instead require the following
supported-operation contracts before enabling remote POSIX in SwordFS:

| Requirement | Actual evidence required |
| --- | --- |
| R1 traditional POSIX owner | Two independent Mounts, independent process owners and `fork`; kernel owner from the real callback; not PID or `fh` substitution |
| R2 range correctness | Cross-Mount `F_GETLK`, `F_SETLK`, blocking `F_SETLKW`, shared/read conflicts, overlap, partial unlock and conversion |
| R3 close semantics | Closing **any** process-owned fd for same inode releases that process's traditional POSIX ranges; unrelated owner's locks preserved; normal `FLUSH` and final `RELEASE` observed |
| R4 cancellation/failure | EINTR before grant, no post-cancel new grant, Owner Lease loss fails closed on lock calls while normal I/O remains usable, no forced umount |
| R5 capability | Only `FUSE_CAP_POSIX_LOCKS` is added to already-shipped `FUSE_CAP_FLOCK_LOCKS`; real negotiated two-Mount kernel-to-Redis authority verified; no apparent local-only success |
| R6 unsupported API boundary | OFD remains documented unsupported. Add a limited explicit **characterization** test with `F_OFD_*` to record existing kernel response, possible stale owners and effects on a supported POSIX caller; **do not** turn it into an OFD conformance gate or infer that it must be implemented |
| R7 conformance | Promote `generic/131` only with real Redis-authoritative result; `generic/478` mixes unsupported OFD and remains outside supported classification |

**Implemented in PR #494:** `Vfs.cpp` negotiates **both**
`FUSE_CAP_POSIX_LOCKS` and `FUSE_CAP_FLOCK_LOCKS`, rejecting unsupported
remote capability rather than allowing a kernel-local fallback. It reuses
`FuseAdvisoryLocks::SetPosix/GetPosix/UnlockPosixOwner` and
`RedisLockAuthority`. The `SwordFsFlush` path attempts per-process Owner
unlock on close, but a short Fiber-safe local `(inode, kernel_owner)`
attempt hint skips Redis entirely for files/owners that never attempted a
record-lock grant; a result-unknown attempt remains eligible for cleanup.
Backend unlock failure does not turn a successful ordinary data FLUSH into
an I/O error. No per-OFD state, owner classifier, Redis schema, fallback
lock service or separate thread pool is introduced.

**Risk boundary:** we can promise correct distributed behavior for
**traditional POSIX callers** on supported normal paths once tested.
Because the untagged kernel transport cannot reliably reject unsupported
`F_OFD_*` requests, we **cannot promise isolation against arbitrary
coexisting OFD callers or complete OFD cleanup**. This must be in the
feature documentation, and any observed persistent contamination must be
evaluated before activation rather than ignored as merely “unsupported”.

## 4. Workflow / decision

### #494 strict-review remediation — bounded cleanup of the supported POSIX namespace

#### Owner lifetime correction after strict Review (2026-10-19)

Two further lifecycle regressions were reproduced by the **test-first**
[Debug run 39216197448](https://github.com/SwordInfra/SwordFS/actions/runs/39216197448):

- **Stale handle ABA:** after a successful process-owner `FLUSH`, the local
  Owner hint was erased but the final `RELEASE` for that handle could arrive
  later. Creating another Owner hint previously restarted its `attempt`
  counter at 1, so the old `RELEASE` could mistake a new same-Owner lock for
  its own lock and send an incorrect Redis unlock. Attempt tokens are now
  **monotonic across deletion and recreation for the full Mount lifetime**,
  issued while holding the existing `posix_owner_mutex_`. Handle snapshots
  are compared against this unique token before any remote unlock.
- **Old failed-close cleanup hidden by a new grant:** after a full-owner
  unlock failed, a successful non-overlapping `SETLK` previously cleared
  `cleanup_pending`, even though Redis could still contain ranges belonging
  to the closed FD. Under the existing per-Owner mutation stripe, a new
  mutation must now **complete and confirm the pending full-owner cleanup
  first**. On failure, leave the pending hint intact, schedule the existing
  cleanup Fiber and fail the new request rather than publishing a grant on
  top of stale ranges. On successful cleanup, remove the old hint, then
  record the new unique attempt before issuing the new mutation. An
  in-flight background retry uses the same stripe and checks the old token
  before acting, so it cannot erase the new grant.

These guards use the existing FiberMutex stripes, Redis authority and
retry coordinator. They add **no Redis operation for a never-locked
`FLUSH`**, no persistent state, no new thread pool, and no OFD feature.
The two regressions are tested with a delayed final RELEASE across Owner
recreation and a failed-close → failed fresh grant → successful cleanup →
fresh disjoint grant sequence.

The independent pre-merge review identified three **reproduced** regressions
with RED evidence, not reasons to implement OFD semantics:

- A supported POSIX record lock was stranded when an unsupported
  `F_OFD_SETLK` reached the same FUSE callback and its final close did not
  carry the corresponding opaque owner. The #428 wire contract still lacks
  an OFD discriminator. **Defensive** cleanup now records a lock-attempt
  Owner against the submitting *FUSE file handle* and on final `RELEASE`
  unlocks that Owner only if the recorded attempt remains current. A normal
  process-owner `FLUSH` still takes precedence, so the fallback does not
  advertise any OFD ownership, unlock conversion or conformance contract.
  Handles that never attempted a record lock cause **zero Redis calls**.
- A failed final-close unlock while the Mount Owner Lease remained active
  left a Redis grant without a future `FLUSH` to retry it. After a failed
  owner cleanup the short Mount-local hint is marked pending; a single
  existing FiberRuntime coordinator periodically retries pending owners
  through the existing BlockingExecutor, without another `close()` or
  dedicated thread pool. It stops upon confirmed cleanup, terminal lease,
  or explicit FUSE destroy. The normal data `FLUSH` reply is unaffected.
- A `FLUSH` could submit its full-range Redis unlock while an earlier
  same-Owner grant was still in flight. The hint's revision did **not**
  serialize those remote changes. A bounded set of existing `FiberMutex`
  stripes now guards the **actual authority mutation** by `(inode, owner)`;
  the stripe is released before any blocking `F_SETLKW` scheduler wait.
  The revision is retained only to distinguish stale handle/cleanup
  snapshots from newer grant attempts.

These local hints are volatile, *not* authoritative lock metadata. A failed
remote mutation may have committed and must not be mistaken for a no-op;
terminal owner leases still rely on Redis lease expiry and lazy stale-owner
cleanup. The defensive `RELEASE` path is solely to prevent unsupported
requests from indefinitely contaminating **supported POSIX callers**; it is
not OFD support. No automatic unmount, new persistent schema, global retry
journal or unrelated I/O Redis work is introduced.

### Verified activation and upstream conformance (PR #494)

TDD RED on [run 38869059555](https://github.com/SwordInfra/SwordFS/actions/runs/38869059555)
confirmed that two independent kernel Mounts without POSIX capability
negotiation missed an overlapping remote lock. The subsequent GREEN
implementation passed the real two-Mount conflict, shared/read compatibility,
process any-close, blocking `F_SETLKW`, signal interruption and revoked-lease
tests in [run 38880840152](https://github.com/SwordInfra/SwordFS/actions/runs/38880840152).

The pinned fstests upstream `generic/131` invokes the POSIX advisory lock
test (`_require_test_fcntl_advisory_locks` and `_run_locktest`). Its **raw
result is PASS** on that run with `FUSE_CAP_POSIX_LOCKS` enabled. The old
`KERNEL_LOCAL_ONLY` label was stale: `generic/131` itself exercises one
Mount, but the separate two-Mount E2E verifies distributed Redis authority.
The two pieces of evidence together justify moving **exactly**
`generic/131` from `known-gaps.tsv` to `supported.txt` and subjecting it to
the normal blocking supported-regression gate. This does not establish
support for OFD or for other unreviewed fstests.

`generic/478` exercises explicitly unsupported OFD lifetime semantics;
it stays in `deferred-ci.tsv`, not `supported.txt`, and is never counted
as a PASS. Capability activation and conformance promotion remain subject
to #494's independent pre-merge review and post-merge main CI.

### CI regression correction for default-on remote POSIX (2026-10-16)

The first full GREEN run exposed a critical fast-path problem: the low-level
FUSE `FLUSH` callback occurs even when the file has never used record locks.
Calling Redis `SetPosix(F_UNLCK)` on every ordinary close adds a backend RTT
and contention to unrelated data operations. The pinned fstests
`generic/531` stresses thousands of unlinked temporary file closes and
timed out at 600 seconds after POSIX capability activation; the historical
baseline was already long-running, so this is strong regression evidence,
not proof of the sole cause.

The `FuseAdvisoryLocks` adapter therefore tracks **only mount-local owners
that have attempted a non-unlock POSIX request**, keyed by `(inode,
kernel_lock_owner)`. This is an **optimization/conservative cleanup hint**,
not the authoritative lock table. It is populated **before** dispatching
the backend attempt because an uncertain reply is not proof of an absent
Redis mutation. On `FLUSH`, owners absent from this hint need no Redis
round trip. Tracked owners still receive a full-range Redis unlock; after
a confirmed unlock the entry is removed only if no concurrent new lock
attempt advanced its counter. On transient unlock failure the owner remains
tracked for another delivered close. The adapter does not introduce
per-inode I/O transactions, Redis schema changes, or persistent state.

Separately, upstream `generic/478` **specifically tests OFD lock lifetime**
using `clone/dup/close` and is explicitly outside the supported SwordFS
feature set (#464 closed/not planned). With POSIX remote negotiation its
previous *kernel-local-only* PASS is no longer a valid expectation; it
stalled at the 900-second bounded-suite limit. Preserve the selected test
and historical observation, but mark `generic/478` **DEFERRED_CI** with
the required `conformance-deferred-change` approval label and the
unsupported-OFD rationale. Do not report a PASS or treat this as supported
conformance. No OFD implementation or classification heuristic is added.

- The prior blanket “protocol BLOCKED, do not implement” conclusion is
  superseded; supported traditional POSIX-only RED/GREEN is complete.
- Real two-Mount FUSE RED and GREEN, Debug/Release E2E, upstream
  `generic/131` PASS, and the full PR CI are verified. `generic/131` is
  promoted to `supported.txt` with a blocking conformance regression gate.
- The untagged OFD transport limitation remains real. If independent
  review or bounded unsupported-OFD characterization identifies concrete
  harmful interaction with supported POSIX operations, evaluate that
  evidence without implementing OFD or asserting unsupported conformance.
- Remaining lifecycle gates: independent review of #494, per-file C++
  coverage confirmation, and main CI after merge.
- No #464 reopening or OFD feature implementation. No forced FUSE umount.
