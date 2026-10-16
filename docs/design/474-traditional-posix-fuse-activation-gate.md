# #474 — Activate traditional POSIX record locks: JuiceFS peer review

**Updated design decision (2026-10-16):** retract the earlier blanket
“POSIX activation blocked until reliable OFD rejection” conclusion.
The kernel still does **not** provide a reliable POSIX-versus-OFD request
class discriminator. However, **that absence alone is not a valid reason
to disable traditional POSIX record locks altogether**: JuiceFS forwards
them through the same Linux FUSE interface today. The supported-product
contract and unsupported-OFD limitations must be assessed separately.

**Scope:** enable *traditional* process-associated `F_GETLK`,
`F_SETLK` and `F_SETLKW` through the existing SwordFS Redis lock
authority once actual two-Mount tests and lifecycle gates are green.
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

**Current code base:** `FuseAdvisoryLocks::SetPosix/GetPosix/
UnlockPosixOwner` and `RedisLockAuthority` already exist; `Vfs.cpp`
currently negotiates remote BSD flock and explicitly unsets
`FUSE_CAP_POSIX_LOCKS`. `SwordFsFlush` already unlocks the kernel
process owner (when POSIX negotiated) after local cleanup. Reuse these
paths first rather than copying JuiceFS's extra handle bookkeeping
preemptively. Revisit bounded fallback cleanup *only if* testing shows
a present traditional-POSIX correctness or lost-FLUSH problem; do not
introduce per-OFD state, owner classifier, Redis schema or another
thread pool by default.

**Risk boundary:** we can promise correct distributed behavior for
**traditional POSIX callers** on supported normal paths once tested.
Because the untagged kernel transport cannot reliably reject unsupported
`F_OFD_*` requests, we **cannot promise isolation against arbitrary
coexisting OFD callers or complete OFD cleanup**. This must be in the
feature documentation, and any observed persistent contamination must be
evaluated before activation rather than ignored as merely “unsupported”.

## 4. Workflow / decision

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

- **Design review reopened / ready for test-first verification**, not a
  declaration that the remote feature already passes. The prior “protocol
  BLOCKED, do not implement” conclusion is superseded.
- Perform TDD with real two-Mount Linux FUSE tests: RED while POSIX
  capability is disabled; GREEN by the smallest safe negotiation/cleanup
  change using existing #431/#471/#472 facilities; no local compilation.
- Investigate any **actual** unacceptable interference discovered by the
  limited OFD characterization. If it cannot be bounded, document a
  concrete blocker with a failing reproduction instead of repeating the
  abstract assertion that “missing type tag => traditional POSIX disabled”.
- If #474 is activated, rerun Debug/Release E2E, actual fstests generic/131,
  conformance classification, per-modified-production-C++-file patch
  coverage >90%, independent review, and main CI after merge.
- No #464 reopening or OFD feature implementation. No forced FUSE umount.
