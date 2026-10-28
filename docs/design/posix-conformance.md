# POSIX conformance

SwordFS uses upstream `pjdfstest` as a semantic conformance signal. The goal is
not to hide currently unsupported behavior or require the whole upstream suite
to be green while SwordFS is still in beta. The conformance system separates
observation from the regression contract so capability debt stays visible and
already-supported semantics cannot silently regress.

## Namespace component limits

SwordFS treats the maximum pathname-component length as a metadata namespace
invariant rather than a syscall-specific rule. A single component may contain
at most 255 bytes. Every metadata entry point that accepts a directory-entry
name validates that component before ordinary lookup or mutation, so an
overlong component returns `ENAMETOOLONG` instead of being collapsed into a
later error such as `ENOENT`.

The rule applies consistently to lookup, create, mkdir, unlink, rmdir,
symlink, hard-link destination names, and both source and destination names of
rename. Metadata entry points share the same limit and
validation contract; backend-specific storage or lookup behavior must not
change this error precedence.

## Special-node creation

SwordFS treats `mknod` as one metadata-backed namespace creation operation,
not as a collection of FIFO/device/socket-specific paths. The backend-neutral
contract accepts the complete inode mode and `rdev`, creates the inode and
dentry atomically, and returns the authoritative inode used for the FUSE entry
reply.

The accepted `mknod` inode types are regular files, FIFOs, character devices,
block devices, and socket nodes. Directories and symbolic links remain owned
by `mkdir` and `symlink`; unknown file-type encodings are rejected rather than
persisted. Character and block devices persist the supplied `rdev` exactly.
All other accepted node types canonicalize `rdev` to zero because device
identity has no meaning for them.

Mode ownership follows the negotiated low-level FUSE contract. Volumes
formatted with `--enable-posix-acl` request `FUSE_CAP_POSIX_ACL` and
`FUSE_CAP_DONT_MASK` only as a pair. With the pair active, the kernel supplies
the unmasked create mode and caller umask, and metadata applies that umask
exactly once when no parent default ACL exists. Volumes without the persistent
feature, or mounts where the pair is unavailable, keep POSIX ACL xattrs
disabled and request setup normalizes the already-kernel-applied umask to zero
before metadata creation. Linux VFS/FUSE with `default_permissions` remains the
ordinary DAC/ACL authorization authority; metadata owns only persistence,
inheritance, mode synchronization, and namespace/state invariants.

Special nodes reuse the ordinary inode/dentry lifecycle. They inherit the
current creation uid/gid rule, start with one link, use zero size, participate
in lookup/readdir/stat through their persisted mode, and follow the existing
unlink/orphan/reclaim machinery. Persisting a socket inode represents the
filesystem namespace object created by Unix-domain-socket `bind`; socket data
transport remains kernel-owned.

## CREATE reply ownership

The low-level FUSE `CREATE` operation commits the named inode and directory
entry before registering a local `FileHandle` and provisionally retaining one
inode lookup reference. Delivery of `fuse_reply_create` determines whether the
kernel acquired ownership of these local resources:

| Reply result | Lookup reference | Local file handle | Committed named entry |
| --- | --- | --- | --- |
| Success | Published until `FORGET` | Registered until kernel `RELEASE` | Preserved |
| Failure | `PublishRetainedLookup` rolls back one reference | The CREATE hook releases/unregisters the unpublished `fh` | Preserved |

A reply transport failure must not unlink the created name, undo metadata,
or send another FUSE reply. The reply helper owns the provisional lookup
rollback; the FUSE hook owns cleanup of the unpublished file handle. Release
still unregisters the handle when its final flush/close returns an error; that
error is diagnostic after the reply attempt, not another syscall reply.

The `O_TMPFILE` CREATE-style reply uses the same ownership split, except its
metadata commit has already unlinked the generated name and published an
anonymous orphan for normal reclamation.

## OPEN and OPENDIR reply ownership

An ordinary low-level FUSE `OPEN` or `OPENDIR` registers a local handle before
`fuse_reply_open` attempts to deliver the descriptor to the kernel. Unlike
`CREATE`, neither hook retains a new FUSE lookup reference. Reply delivery
determines who is responsible for releasing the handle:

| Hook | Reply delivered | Reply delivery failed | Lookup / metadata |
| --- | --- | --- | --- |
| `OPEN` | The kernel owns `fh` until `RELEASE` | The request fiber calls `VfsImpl::Release` to unregister the unpublished `FileHandle` and drop its inode open reference | No additional `nlookup`; existing open-time effects (including atime or `O_TRUNC`) remain committed |
| `OPENDIR` | The kernel owns `fh` until `RELEASEDIR` | The request fiber calls `VfsImpl::ReleaseDir` to unregister the unpublished `DirHandle`, allowing its iterator to be destroyed | No additional `nlookup` or namespace mutation |

The failed-reply cleanup happens synchronously in the original request fiber.
The transport has already attempted the one permitted FUSE reply, so the hook
does not send another error reply, roll back metadata, or adjust lookup counts.
If the initial VFS open operation fails before registering a handle, the hook
instead replies with that error and performs no release. Regular-file release
uses the existing final-close flush: an unexpected flush error is logged, but
the handle and inode open reference are still relinquished.

## Distributed advisory locks (#379 — design contract)

This section defines the **first-stage** cross-mount locking contract.
#429/#430 Redis lease and lock authority, #431's POSIX FUSE adapter, and
#432's nonblocking BSD flock callback and final-RELEASE cleanup are implemented.
Before #473, both remote FUSE lock capabilities were disabled: earlier flock
success (including fstests `generic/504`) was kernel-local. #473 enabled
Redis-authoritative BSD flock. **#474's implementation in PR #494 now also
negotiates `FUSE_CAP_POSIX_LOCKS` by default**, requiring kernel support for
both remote lock capabilities rather than silently falling back to local
locking. The real two-Mount `F_GETLK`/`F_SETLK`/`F_SETLKW`, any-close and
EINTR tests, Debug/Release E2E, and upstream `generic/131` passed at PR
review head `df6fc74` in CI [39216797074](https://github.com/SwordInfra/SwordFS/actions/runs/39216797074)
(22 successful jobs, 2026-10-19). **At that pre-merge checkpoint**, #494 was
Ready for Review; this records test evidence, not a live `main` deployment
status. The earlier blanket OFD transport blocker was
withdrawn after comparing JuiceFS; OFD remains unsupported and not planned
(#464 closed), and this activation does not implement OFD semantics.

### Implementation decomposition and handoff

Parent #379 owns this cross-class architecture contract, integration,
conformance classification and final closeout; its children own bounded
design, implementation, test and review evidence. Use the repository's
**native GitHub sub-issue hierarchy**, not an unrelated backlog of PRs:

| Issue | Deliverable | Depends on |
| --- | --- | --- |
| #428 | Verify Linux/libfuse owners, close and interrupt behavior; classify OFD | — |
| #429 | Minimal mount-owner lease identity, liveness and fail-closed loss protocol | #428 for finalized transport identity |
| #430 | Redis atomic lock authority, expired-owner filtering, optional lazy GC and uncertainty recovery | #428, #429 |
| #431 | Traditional POSIX `getlk`/`setlk` and any-close release | #428–#430 |
| #432 | BSD `flock` and open-file-description last-close release | #428–#430 |
| #433 | Interruptible waits, cancellation, cross-mount integration and activation | #431, #432 (and the shared prerequisites) |
| #464 | Closed / not planned: unsupported OFD record locks, no implementation | — |

The #433 integration work is decomposed into native sub-issues:
**#471** common bounded Fiber wait/retry, **#472** FUSE interrupt handling,
**#473** negotiated BSD flock/two-Mount conformance, and **#474** the
traditional POSIX-only remote activation and verification. The verified
FUSE transport cannot distinguish unsupported OFD requests from traditional
record locks. JuiceFS demonstrates that traditional POSIX support can still
be shipped with a clear unsupported-OFD boundary; this requires focused
real two-Mount RED/GREEN and risk-characterization tests in SwordFS.
The updated decision is documented in
[`474-traditional-posix-fuse-activation-gate.md`](474-traditional-posix-fuse-activation-gate.md).
The common wait implementation is reviewed in
[`471-fiber-lock-wait.md`](471-fiber-lock-wait.md).
The reviewed activation contract and required real two-Mount proof for #473
are maintained in [`473-distributed-flock-activation.md`](473-distributed-flock-activation.md).
With #473's remote BSD flock activation, upstream `generic/504` no longer
passes its **kernel-local** `/proc/locks` introspection check. Its exact
`lock info not found` output is tracked as `upstream_not_applicable`, not as
remote flock support: `FUSE_CAP_FLOCK_LOCKS` forwards ownership to Redis and
does not create a local kernel flock table entry. The upstream test checks
the latter explicitly. The real two-Mount kernel-to-Redis E2E matrix, not
this upstream `/proc/locks` test, is the remote BSD flock acceptance gate.
The #473 design removes the temporary `--enable-lock-owner-lease` staging
switch: once activated, every Redis-backed Mount registers its Owner Lease
and requests remote BSD flock by default, with no kernel-local fallback.
An unavailable lock service fails lock requests without automatically
unmounting the filesystem or affecting healthy ordinary I/O.

The actual changes may be reviewed separately as each child reaches its own
design-readiness checkpoint. The parent remains open until Redis integration, relevant fstests
classification, post-merge main CI and review are reconciled. #429 establishes **owner lease validity**, not a general
Redis metadata session or a lock cleanup service. #430 owns actual lock
records, atomic owner-validity checks and optional lazy reclamation. The
#430-specific design review, candidate Redis schema and unresolved gates are
tracked in [`430-redis-lock-authority.md`](430-redis-lock-authority.md).
The traditional POSIX FUSE adapter, per-close FLUSH contract, blocking-worker
boundary, GETLK diagnostics and missing-FLUSH limitation are reviewed in
[`431-posix-record-lock-integration.md`](431-posix-record-lock-integration.md).
The BSD flock FUSE adapter, final-RELEASE cleanup, reuse and capability gates
are reviewed in [`432-flock-fuse-integration.md`](432-flock-fuse-integration.md).
OFD implementation is **not planned** (#464 closed); its untagged shared
transport is a POSIX-only activation constraint under #474, not a request
to implement OFD or reopen #464.
No intermediate child permits enabling incomplete remote-lock support or
claiming OFD/delegation compatibility. The known
conversion/reply-delivery ambiguity is a mandatory #430 authority decision,
then an integration regression case in #433; do not leave it as an orphan
parent-only concern.

### Supported classes and transport

The intended first stage supports traditional **process-associated POSIX
record locks** (`F_GETLK`, `F_SETLK`, `F_SETLKW`) and **BSD `flock`**
(`LOCK_SH`, `LOCK_EX`, `LOCK_UN`, `LOCK_NB`). They are advisory: an application
that does not lock may still read/write. Locks on separate mounts of one
Redis-backed volume must observe the same conflicts. The lock authority
must remain consistent across mounts.

Traditional POSIX locks belong to the **process lock owner and inode**, not
the descriptor used to create the lock. Closing **any** descriptor for that
inode from the same process releases that process's **entire** record-lock
set for the inode, even when other independently opened descriptors remain.
Use kernel-supplied `fi->lock_owner` to identify this owner across `getlk`,
`setlk`, and the `FLUSH` event; PID and mount-local `fh` are insufficient.
`FLUSH` is the per-close signal, not necessarily the last-reference release.
Never skip this release merely because data `Flush` returned an error.

BSD `flock` belongs to the **open file description** and covers the complete
inode. Descriptors sharing it through `dup` or `fork` share the lock; only
the last reference releases it. The FUSE `flock` callback uses kernel
`lock_owner`, and the `RELEASE` callback's `fi->flock_release` conveys the
last-close release. The POSIX record-lock and BSD flock conflict namespaces
are distinct on Linux; treating all locks as one mutex is incorrect.

POSIX record locks admit compatible overlapping read/read holders; any
other-owner range intersection involving a write lock conflicts. Same-owner
relocking, partial unlocking, split/merge and conversion use POSIX semantics.
BSD flock admits shared/shared, rejects other-owner shared/exclusive and
exclusive/exclusive, and applies whole-file conversion. Nonblocking conflicts
use the proper Linux errno (`EAGAIN`/`EACCES` for record locks,
`EWOULDBLOCK` for flock). `GETLK` must use `fuse_reply_lock` and return
`F_UNLCK` or a real conflicting range; the `l_pid` field is diagnostic, not
the ownership key. Never fabricate a globally valid PID when a conflicting
holder lives on another mount/PID namespace.

**Explicitly excluded / not planned:** OFD record locks (`F_OFD_*`) and file
leases/delegations (`F_SETLEASE`, `F_SETDELEG`). OFD is not BSD `flock`; the
selected low-level FUSE `getlk`/`setlk` callback does not expose an explicit
OFD-versus-process lock-kind argument. Do not claim OFD support until
verified on the target kernel/libfuse. #474 no longer demands a nonexistent
OFD discriminator as a blanket prerequisite to the traditional POSIX feature;
the PR #494 capability activation is justified by actual two-Mount
traditional-POSIX RED/GREEN CI. This does **not** provide correct OFD
ownership or cleanup. Upstream `generic/478` is a deliberately unsupported
OFD lifecycle test, selected but explicitly deferred from CI rather than
counted as supported or passed. `generic/786` and `generic/787` require
separate delegation support.
Advisory user locks are not #424's internal Slice write/truncate/grow fence.

### Stable identity and owner lease lifecycle

The backend owns locking independently of chunk-content metadata:

```
POSIX owner = (volume, mount_owner_incarnation, kernel_lock_owner, POSIX)
flock owner = (volume, mount_owner_incarnation, kernel_lock_owner, FLOCK)
lock record = (inode_id, owner, class, mode, byte_range_or_whole_file)
```

Every new mount registers a **never-reused** owner incarnation with the shared
lock authority before any remote lock operation. The first-stage single-primary
Redis design allocates a monotonically increasing per-volume generation via
validated `WATCH`/`GET`/`MULTI`/`INCRBY`/`EXEC`, then creates its own lease
via native `SET NX PX`. These are **two distinct atomic operations, not one
atomic registration**: losing an allocated generation or
leaving an unconfirmed, unused lease is safe, because neither may ever issue
a lock operation. A counter overflow or uncertain authority history fails closed.
Random client IDs alone are not proof of strict non-reuse: if an expired
key disappears, an old `register(id)` replay could otherwise recreate it.
It is **not** a Redis connection/session and does not take over the lifecycle
of unrelated metadata, open files or reclaim work. A restarted daemon never
reuses the incarnation, including after connectivity loss. Existing mount-local
`FileHandle::fh` and `InodeHandle::open_count` are not distributed lock owner
identities. `fh` still validates legitimate inode access.

The lock authority stores:

- One bounded **owner lease key** per mount incarnation: a nonce and
  authoritative Redis TTL. No separate active/revoked flag is needed: an
  absent, expired or explicitly deleted key is **invalid**. Renewal never
  creates a missing lease and cannot change its incarnation. An expired
  identity cannot re-register.
- Per-inode POSIX record locks: canonical owner-keyed, nonoverlapping
  `[start,end]` intervals with read/write mode and optional diagnostic PID.
  End-of-file (`l_len == 0`) has an explicit infinity sentinel; validate
  range normalization and overflow, not physical file size.
- Per-inode BSD flocks: whole-file shared/exclusive owner records.
- Optional owner-to-inode reverse index for bounded physical cleanup. It is
  **not** required for *logical* expiration and is not a second source of
  ownership truth; #430 chooses an index only if needed to bound stale data.
- Per-owner operation IDs/outcomes for uncertain-commit replay and a
  per-inode lock-change sequence for waiters.

`GETLK`, range-set/unlock, and flock operations have backend-neutral
semantic interfaces. VFS must not decode Redis lock keys or implement a
parallel client-local authoritative lock table. Locks can survive unlink
while open handles exist; reclaim must not reuse an inode identity with
stale live locks. Distinguish POSIX owner cleanup at every qualifying
`FLUSH` from flock cleanup at `flock_release`/last `RELEASE`.

#### #429 owner lease: chosen minimal contract

**Why any lease?** Explicit `F_UNLCK`, delivered POSIX `FLUSH`, and flock
final `RELEASE` already cover normal close. A dead mount holding a persistent
distributed lock never sends those callbacks: without any expiration or
external invalidation, another mount can be blocked indefinitely. Therefore
**eventual logical invalidation** of orphaned owners is required for lock
availability; immediate physical deletion and a Redis-wide session cleanup
service are **not** required for that correctness property.
Crucially, owner lease expiry only addresses **orphaned mounts**: it cannot
repair a traditional POSIX close whose FUSE `FLUSH` callback was skipped
**while the mount remains healthy and continuously renewing**. That
exceptional-close gap is a separate #431 transport/semantic blocker; do not
claim a mount lease provides bounded per-inode/per-process close cleanup.

| Candidate | Evaluation |
| --- | --- |
| Incarnation only, no validity mechanism | Reject: prevents identity reuse but cannot determine when an orphaned lock stops conflicting. |
| Mount-owner lease, one renewal per mounted client | **Choose for first stage**: bounded orphan lifetime without per-lock renewal or per-data-I/O metadata RTT. Expired lock records remain inert when the authority evaluates conflicts. |
| One independent lease per lock | Defer: scales renewal and timeout state with held locks, creates unnecessary distinct expiry moments, and still cannot fence an application's critical section by itself. |
| JuiceFS-style heartbeat plus eager stale-session sweep | Reference only: its session also owns non-lock resources; mandatory eager scan/delete and its concurrency model are not necessary for our first-stage lock semantics. |

**Authority-side protocol** (first stage: Redis 7.x single primary with no
automatic failover; exact key namespace is owned by #430):

1. `RegisterOwner` allocates a new per-volume numeric generation using
   native `WATCH owner_generation_key`, a validated `GET`, then
   `MULTI`/`INCRBY 1`/`EXEC`. Parse the **previous** value strictly as a
   canonical nonnegative integer below `INT64_MAX` before enqueuing the
   increment; a missing, malformed, negative or exhausted counter fails
   closed without mutating the authority. WATCH prevents concurrent owner
   allocation or counter removal from invalidating the validated snapshot.
   The existing `RedisMetaClient::Transact` and `RedisKvTxn::IncrBy` provide
   this protocol; the Lua-based `IncrNonNegative()` is not needed.
   After a confirmed generation increment, issue a **single**
   `SET owner_lease:{generation} <incarnation_nonce> NX PX <lease_ttl_ms>`.
   Counter allocation and lease creation are intentionally separate
   operations: a lost response never authorizes an unconfirmed generation;
   abandon any unconfirmed owner attempt rather than retrying SET against
   the same key. A collision is an authority-integrity failure. An instance
   registers at most once and must not silently adopt another generation.
   No client-supplied `register(old_incarnation)` endpoint exists; abandoned
   lease keys expire normally. Lost/rolled-back generation history remains
   unsupported and requires fail-closed.

   **Redis-only beta format invariant:** `RedisMetaOps::FormatVolume` writes
   the non-expiring `owner_generation=0` in the **same transaction** as the
   volume `format` key. Registration must not create, repair or migrate a
   missing or invalid generation counter. A previously formatted volume
   without this key must be reformatted before enabling shared locks;
   ordinary non-lock I/O remains independent of lock initialization errors.
2. `RenewOwner(generation)` uses native `PEXPIRE owner_lease:{generation}
   <lease_ttl_ms> XX` (Redis 7.x). `XX` requires an existing TTL and does **not**
   create a missing lease; expiration or explicit revocation therefore
   cannot be undone by a late renewal. Return `1` confirms extension, `0`
   is terminal loss, and a missing reply is **unknown**, not success. A
   renewal queued before local cutoff may complete late: it cannot revive a
   Redis-expired key, and the client still fails closed if confirmation
   arrives after its conservative deadline. Serialize renewal and shutdown
   locally; do not issue new renewals in terminal state.
3. `RevokeOwner(generation)` first marks the client terminal, then issues
   native `DEL owner_lease:{generation}`; deleting the key is an idempotent
   authoritative invalidation for this **never-reused** key. An uncertain
   `DEL` result cannot be interpreted as renewed validity; the remaining
   TTL bounds its lifetime. No periodic cleanup is required for expiry.
4. `TryLock`/`Getlk`/`Unlock` (#430) evaluate requesting lease presence/TTL
   and **each potentially conflicting record's** owner lease in one
   optimistic `WATCH`/`MULTI`/`EXEC` validation-and-mutation domain. Every
   key whose validity is used must be watched **before** it is read, including
   leases for all relevant holders and the requesting owner; the per-inode
   lock key, operation results and any affected indices also participate.
   Require positive `PTTL` (a missing/nonexpiring key is invalid) before
   `MULTI`. Starting with Redis **6.0.9**, watched-key expiration is checked
   at `EXEC` and aborts the transaction even without a competing write;
   Redis 7.x is the chosen test/deployment floor. Renewal/revoke/expiry or
   another lock mutation makes an optimistic snapshot stale and requires
   bounded retry. Expired/revoked owners are inert for conflict and GETLK.
   Stale unlock/GC can affect only their exact old grant identity. **This
   WATCH proof needs targeted Redis 7.x concurrency tests in #430 before
   declaring its implementation ready; do not silently fall back to a
   separately sampled client/server clock.** If the proof or performance
   bound fails, introduce a small, scoped Lua transaction for **#430's lock
   grant path**, not an unnecessary Lua-based #429 heartbeat service.
5. Physical reclamation of inert lock records or indices is best-effort and
   must be bounded, identity-conditional and safe to repeat. #430 must bound
   read amplification and retained stale records (possibly through lazy GC)
   without making immediate cleanup a precondition for new grants.

**Lease timing: initial normative defaults** (configuration bounds and
operational review are part of #429 TDD):

| Parameter | Default | Rationale / validity rule |
| --- | --- | --- |
| Owner lease TTL | **60 s** (`PX 60000`) | Bounded orphan validity once the last renewal reaches the server; no lock-by-lock TTL. |
| Heartbeat period | **10 s** | Dedicated mount-lifetime control worker, independent of application lock traffic; jitter/bounded retry must not postpone local cutoff. |
| Local safety reserve | **15 s** | Self-stop no later than `CLOCK_BOOTTIME(send) + 45 s` after the **last confirmed** create/renew dispatch. Covers an explicit assumed bound on Redis wall-clock acceleration/step and local clock drift; not arbitrary unbounded clock adjustments. |
| Redis I/O budget | **5 s per individual Redis command's I/O phases** | `--lock-lease-io-timeout` caps pool-wait, connect and socket timeouts individually to at most one-third of this budget (and disables internal transaction retries). This is **not** a single end-to-end deadline spanning registration, renewal, or unmount. |

The client samples **Linux `CLOCK_BOOTTIME` at request dispatch**, not at
response arrival, and after a confirmed registration/renewal sets
`local_cutoff = send_boottime + 60 s - 15 s`. A delayed success after the
previous cutoff must **not** rescue a terminal mount; a success observed
before cutoff may extend confidence only up to its *own dispatch-based*
cutoff. Failed, negative or otherwise invalid local clock samples must
immediately fail the lock-only owner state closed; clock recovery never
reactivates a terminal generation. `CLOCK_BOOTTIME` accounts for suspend, unlike an assumed
`std::chrono::steady_clock`. Redis key TTL uses its **own wall clock**;
the 15-second reserve is a bound **only when Redis clock steps/drift and
host suspend behavior obey the documented deployment assumptions**. An
unbounded forward Redis wall-clock jump can expire a lease before any
independent client notices; no finite local margin can solve this. This
requires operational clock discipline/monitoring or stronger resource-side
fencing, not a claim of guaranteed critical-section safety.

Transient heartbeat failure **before** the cutoff does not by itself mean a
lease was revoked. Retry with bounded backoff, using a single in-flight
renewal at a time; **unknown outcomes do not reset local confidence**. A
subsequent confirmed `PEXPIRE XX` on the same unexpired generation is
permitted before terminal cutoff, since renewing the same key does not
duplicate grants. A late success after the cutoff never reactivates local
state. Redis return `0`, ambiguous primary history, or reaching cutoff
enters a one-way terminal fail-closed state. Normal unmount also enters
terminal state before `DEL`. **No silent re-register or resume under old
handles.** Client-local abandonment is **not** a successful Redis unlock;
old requests/cleanup must not affect newer owner generations.

**Mount handoff:** #429 defines an owner-lease guard with states `UNREGISTERED
    -> ACTIVE -> TERMINAL` (no `TERMINAL -> ACTIVE`), scoped to a single mount
incarnation. The mount registers its Owner Lease before advertising FUSE
readiness; with both remote lock capabilities negotiated, this remains a
mount-lifecycle operation, **not** an extra Redis request on ordinary I/O or
on a `FLUSH` where no POSIX Owner has attempted a lock. The blocking/control-thread
heartbeat uses the existing execution-domain boundaries; publish a terminal
atomic state visible at **FUSE lock callback admission** and again **before a
successful lock-operation reply**. Post-terminal new lock requests fail with
EIO, not falsely acknowledged. **A lease-loss event does not call
`fuse_session_exit` or trigger unmount.** Ordinary FUSE requests and healthy
metadata/data I/O continue. Only after a user-initiated unmount does the existing Mount teardown
clean up the FUSE session and best-effort revoke the active Owner Lease
(mark it terminal, stop renewal and attempt bounded DEL of the exact
lease key). #471 does not add a broad direct-process-exit branch.
No enumerating per-inode lock records or
invented asynchronous cleanup guarantee; already executing application I/O
cannot be retroactively fenced. The heartbeat must never call libfuse reply
APIs or borrow a destroyed mount/session reference. Existing FUSE
`RunFuseInFiber` and mount INIT/destroy paths need integration tests before
readiness is claimed.

**Lease configuration after #473:** The `mount` command registers an owner
lease by default on every Redis-backed mount and offers four independent
duration controls: `--lock-lease-ttl` (60s),
`--lock-lease-renew-interval` (10s), `--lock-lease-safety-margin` (15s) and
`--lock-lease-io-timeout` (5s). Positive integral `ms`, `s` and `m`
suffixes are accepted; combinations must leave room for one bounded renewal
before local cutoff. **I/O timeout semantics:** registration performs several
sequential Redis protocol steps (WATCH, GET, EXEC and a separate SET), so a
5-second per-command I/O budget does **not** mean registration completes
within 5 seconds. We conservatively split each command's allowance over
pool wait, connect and socket response; the Redis client has a single-slot
pool and one transaction attempt. A normal shutdown can wait for at most
one in-flight heartbeat PEXPIRE, then issue one native DEL with no retry.
Under operating-system/redis++ timeout behavior, each of those individual
commands is transport-bounded; the sequential total may exceed the
configured I/O budget. No hard wall-clock deadline or arbitrary kernel
stall bound is promised. The local grant cutoff is independently checked
and never extended by an unconfirmed or late Redis response.

The default Mount path registers a Redis generation **after
volume initialization and before FUSE mount**, with the owner heartbeat
independent from ordinary I/O. A failed lease registration is terminal for
that mount's *lock authority*, not its ordinary FUSE service. The normal
FUSE loop exit joins the heartbeat and revokes only its own generation
before session destruction; a Redis failure during this bounded cleanup
falls back to expiry. #430 supplies server-authoritative BSD flock checks;
#473 negotiates FUSE BSD flock by default; #474 (PR #494) also negotiates
traditional POSIX record locks. The native Redis generation counter is
required for lock use on beta volumes; no compatibility migration is attempted.

**Crash/partition/safety boundary:** if the process is paused indefinitely
or the OS permits applications to modify other resources after their advisory
lease expires, the authority cannot guarantee that two application critical
sections never overlap. Even a suspend-aware cutoff cannot stop an already
running application instruction. Advisory locks do not fence ordinary
read/write/truncate or external side effects. #429 guarantees eventual
owner invalidation and no *new authority grants to an expired owner*; it
does **not** promise strict application fencing. A resource-side fencing
token/data mutation protocol, if needed, is separate from #429/#430 (see
#424 for SwordFS write-ordering concerns). Redis rollback/failover that loses
acknowledged lease/lock history is also outside a lone Redis authority's
guarantee: prohibit grant continuation when authority lineage is uncertain.

**Native Redis decision and #430 handoff:** do **not** add Lua to #429.
Native `WATCH`/`MULTI`/`INCRBY`/`EXEC`, `SET NX PX`, `PEXPIRE XX`, and `DEL`
provide the required atomic checks; two-phase registration is safe under the
specified "unconfirmed generation cannot hold locks" rule. Existing
`RedisMetaClient` must gain a **narrow lock-owner-lease adapter** for these
commands; do not put a generic Redis Session service in `IMetaEngine`.
Use the existing `metadata::redis::RedisKey` per-volume hash-tagged prefix
for generation/lease keys, not new unscoped Redis namespaces; namespace
braces alone do **not** constitute tested Cluster support.
`RedisMetaClient::Transact`/`RedisKvTxn` already supply WATCH/EXEC on one
connection and its `OutcomeUnknown` classification, so reuse these for #430
if the **watch-all-relevant-leases-before-read** and expiration-at-EXEC
proof passes. Redis documents this behavior since 6.0.9, and the repo's
`docker-compose.e2e.yml` uses `redis:7-alpine` by default. #429 must not
promise supported Redis Cluster multi-slot transactions: the current
`RedisMetaConfig` supports one Redis endpoint. Fail closed on counter/state
rollback or an untrusted primary transition; Redis `WAIT` or `WAITAOF`
cannot replace a consensus/fencing system. If WATCH proves inadequate under
#430's realistic lock-key shapes, Lua is acceptable **there**, scoped to
atomic lock authorization. Source contracts:
[Redis SET](https://redis.io/docs/latest/commands/set/),
[PEXPIRE](https://redis.io/docs/latest/commands/pexpire/),
[WATCH/EXEC](https://redis.io/docs/latest/develop/using-commands/transactions/)
and [expiry-versus-WATCH](https://redis.io/faq/doc/11ed4zroc6/can-a-key-expire-within-a-multi-exec-transaction).

**Integration and replay details:** per-mount generation allocation and
confirmed registration happen once on every default Redis-backed FUSE Mount.
With #474 (PR #494), the remote POSIX and BSD flock capabilities are **both
required and negotiated**; no kernel-local lock fallback is advertised.
The generation counter
is a durability-critical value, so `FLUSHDB`, failed restoration of that
counter, or Redis failover to stale history invalidates the single-authority
assumption. A routine reconnect to the *same trusted primary* may reissue
`PEXPIRE XX` before local cutoff. A reconnect after suspected history
rollback/primary change **must enter terminal state**; a last-minute
`PEXPIRE` success alone does not prove the history lineage. No automatic
multi-primary failover or Redis Cluster guarantee is advertised. Remote
lock initialization must not add heartbeat overhead to ordinary I/O requests.

**#429 verification boundary:** Redis-backed tests exercise format-time
counter initialization, strict counter rejection, non-reused generations,
single-use `SET NX PX`, renewal and expiry, exact-generation revoke,
known vs unknown Redis replies, injected-clock deadline boundaries, and
terminal old/new epoch isolation. Linux FUSE E2E covers owner revocation
without ordinary mount loss, normal unmount cleanup, invalid timing, and
fresh generation on remount. The opt-in lifecycle does **not** yet send
remote POSIX/flock grants, so no claim is made about FUSE lock-reply cutoff,
blocking waiters, or G2/G4 enforcement by #429 alone. #430 must prove
watch-all-relevant-key TTL expiry at EXEC, old-owner grant rejection,
expired-holder filtering, and safe cleanup of prior generations. No FUSE
remote-lock capability is enabled by #429.

**Peer source review:** JuiceFS `pkg/meta/base.go::{NewSession,refresh}` and
`pkg/meta/redis.go::{doRefreshSession,doCleanStaleSession}` use metadata
client session heartbeats and a stale-session sweep; `pkg/meta/redis_lock.go`
stores `(sid,owner)`-scoped lock records and a reverse index. This gives an
availability-oriented cleanup precedent, **not** proof of strict lock
fencing: the examined grant paths do not visibly atomically gate against
unexpired session status, stale cleanup uses separate key operations, and
refresh may re-create a removed session. Review pinned upstream code at
[JuiceFS 915e831](https://github.com/juicedata/juicefs/tree/915e831c8c94c74c568fde3ecf8a17eb7879d579/pkg/meta)
before making a stronger claim.

### Atomic grant, unlock and ambiguous outcomes

The **linearization point** is a
successfully executed Redis transaction (prefer native WATCH/EXEC once its
expiry predicate is proven; narrowly scoped Lua remains an alternative).
Lock operations atomically:

1. Validate that the requesting owner incarnation has a currently valid
   lease, and exclude expired/revoked competing owners, in the **same
   authoritative time/transaction domain** as the grant (never from a stale
   daemon-local cache or a separate preflight call).
2. Check relevant lock conflicts and class-specific owner replacement,
   insertion, splitting, or removal against the same authoritative snapshot.
3. Mutate the lock record, any chosen owner reverse index and inode change
   sequence together, including any operation identity/outcome for replay.

The chosen Redis atomic operation must cover **all** modified pieces and the
owner-lease time predicate. `WATCH`/`EXEC` alone is **not** evidence of a
server-clock check at the eventual commit point; prefer a verified atomic
script or prove an equivalent protocol. A check-then-write sequence spanning
independent commits is not correct. Redis WATCH conflicts are distinct from
semantic lock conflicts. Limit retries and contention; do not hold a Redis
connection/transaction during an application lock wait. All keys that must
commit atomically must reside in one supported Redis transaction domain.

Give each modifying request a stable operation ID, unique under its owner incarnation.
Serialize still-unresolved modifications to a single
`(incarnation,owner,inode,class)` until their outcome is known. On
`OutcomeUnknown`, retry/reconcile with **the same operation identity** and
the persisted original result; never blindly retry with a new ID or infer
non-application from a matching final lock shape. Define bounded retention
and pruning of the per-owner result journal so late replays cannot execute
an already-acknowledged grant or undo a later one. Optional stale-lock cleanup
is idempotent and guards the complete owner/incarnation identity.

An unsuccessful lock release must be diagnosed and retried safely; ordinary
`FLUSH` can still return its existing data-writeback error, but may not
silently declare remote unlock complete if it failed. `RELEASE` must free
local handle resources even if its remote flock cleanup fails. Redis failover
that loses acknowledged state is **outside** the claimed single-authority
guarantee: mounts must fail closed if the authoritative history becomes
untrustworthy, not assume Redis alone supplies distributed consensus.

### Owner lease expiry and failure model

```
NEW --register--> ACTIVE --confirmed renew--> ACTIVE
ACTIVE --explicit revoke / authoritative expiry--> INVALID
INVALID --optional conditional physical GC--> record removed
```

Invalidity is effective *on expiry at the authority*, not when a cleanup
worker eventually deletes keys. Every renew and lock mutation checks the
same identity and expiry predicate. Stale operations cannot resurrect the
owner or remove newer incarnation records. Physical GC is optional and
separate; an unbounded stale-record accumulation is still unacceptable.
The client must fail closed by its conservative local cutoff and not resume
under old handles. Redis lease expiry alone cannot fence a partitioned
application's pre-existing critical section; the explicit scope/limit is
defined above.

### Waiting, wakeup, cancellation and reply ownership

**#471** implements a single logical **Mount-owned `LockRetryScheduler`**
shared by traditional POSIX and BSD flock requests. A blocking request
makes one immediate #430 nonblocking Redis try-lock; after a genuine
`EAGAIN`, it registers with the Mount scheduler and suspends its Fiber on
its own `utils::FiberBaton` (not a Redis worker). On each common Folly
timer tick, a coordinator Fiber runs **read-only, grouped** lock-state
probes for waiting `(inode, lock class)` groups using the **existing**
BlockingExecutor and #430's authoritative Holder/Owner Lease validity
rules. It compares exact requested ranges and SH/EX compatibility,
then **notifies only** requests that *might* be grantable. Each awakened
**original request Fiber** executes its own existing #430 atomic
`SetPosix`/`SetFlock`: **the Scheduler never grants, reserves, converts,
unlocks or otherwise changes a Redis lock**. If another Mount races and
the original try-lock returns `EAGAIN`, the request simply re-registers.
The read-only probe never promises a grant. Grouped reads and precise
wakeups, rather than timer sharing alone, are what coalesce requests.
No long-running WATCH transaction, busy polling, durable Redis waiter
queue, custom timing wheel, Redis Pub/Sub, inode version/notification
protocol or strict FIFO is required. A **minimal pure read-only group
inspection** may be needed because `GetPosix` is a per-request lookup
with lazy cleanup and `GetFlock` does not exist. It must reuse #430's
validity semantics and must not alter existing grant APIs or storage.
Redis pipelining of read-only group probes can be evaluated, but no
batch-mutation/authorization protocol is needed. Multiple FUSE worker
FiberRuntimes have distinct EventBases: cross-thread scheduling, Baton
lifetime, cancellation
and callbacks must be safe. Compatible shared readers and disjoint POSIX
ranges must not be indefinitely starved by over-coarse inode batching.
A blocking call may wait indefinitely under normal operation, but resource
admission, retry rate and unmount response time must be bounded.

The shared request-cancellation framework (#477) enables FUSE interrupt
reception with `conn->no_interrupt = 0`, independent of lock activation.
`fuse_req_interrupt_func` registers without losing already-arrived interrupts.
Its callback only requests cancellation on the Folly source; it must not
acquire a fiber mutex or use request memory after unregister/destruction.
The lock operation owns its cancellation callback and safely dispatches its
waiter wakeup through `FiberRuntime::Post`. The request fiber coordinates
exactly one reply and interrupt-handler removal:

**Only user/operator action initiates umount.** Lock errors, long
waits, Redis failures, lease expiry and cancellation may never trigger
unmount. #471 retains the existing FuseSessionGuard / RunMount cleanup,
including FiberRuntime shutdown and Owner Lease revoke; it introduces
no broad _Exit(0) path. #473 has enabled negotiated remote BSD flock
with explicit user-unmount lifecycle tests. #474 (PR #494) has now validated
traditional POSIX remote grants, close cleanup, waiter wake and interrupt
against two real Mounts; neither
lock path justifies mass cancellation or automatic unmount.

**#477 already implements** FUSE interrupt-to-token forwarding, the
`fuse_req_interrupt_func` registration/unregistration lifecycle and
`FiberRuntime::Post()`. **#472** adds lock-specific EINTR/grant-vs-signal
handling; #471 does not activate remote POSIX or BSD flock capabilities.
The request-owning Fiber remains responsible for the lock reply; the #477
reply adapter unregisters the FUSE interrupt callback. Cancellation **before
submitting the next attempt** prevents that new grant attempt; an already-
submitted attempt may still confirm a grant and win a concurrent signal
as the tested Linux FUSE contract permits. No blanket rollback is
valid when the original lock was converted or another owner has since
acquired a compatible lock.

The **#471/#472 cancellation seam** consumes the request-level
`folly::CancellationToken` from `SwordFsContext`, with an operation-owned
`folly::CancellationCallback` and exact `utils::FiberBaton` wakeup. The
Scheduler itself has no cancellation state or `CancelWaiter()`. Race-safe
registration and unregistration are owned by the request Fiber. A request
cancelled before registering must not sleep;
one cancelled during a Scheduler read-only probe must not be notified
into a destroyed waiter or initiate another TryLock. A request cancelled
after the probe's notification but **before its next try-lock is issued**
must not submit the try-lock. By contrast, a signal arriving while the
request's **already-submitted Redis mutation** is still executing cannot
justify an immediate `EINTR`: only the request Fiber may reply, **after**
the in-flight attempt result is known. A confirmed success may beat a
late interrupt; `EAGAIN` or other ungranted outcome may terminate the
cancelled request with `EINTR`. Neither the read-only Scheduler nor the
interrupt callback replies, reserves a grant or modifies Redis locks.
The cancellation token stays cancelled across retries; Folly Baton reset
requires no concurrent poster. User-requested umount is separate from per-waiter cancellation;
#471 preserves the existing Mount teardown path.

At the FUSE boundary, the already-interrupted registration callback may run
inline before the request Fiber starts; it only signals the request's
`CancellationSource`. Blocking POSIX `setlk(sleep=1)` and blocking BSD `flock`
must return `EINTR` without starting a new mutating attempt in that case.
After an attempt is submitted, its authoritative result determines whether
a valid grant wins a late signal or an unsuccessful contention result becomes
`EINTR`. During read-only inspection, cancellation can complete the request
without waiting for the probe, and the probe must remain safe to finish after
the waiter retires. Every path unregisters the interrupt handler before its
single consuming FUSE reply, including when `fuse_reply_err` itself fails;
the callback never replies. #472 validates these boundaries through the
low-level FUSE hooks in `tests/unittests/fuse/VfsImplTest.cpp`, using a gated
external lock authority without negotiating either remote-lock capability.

Redis commit and FUSE reply are not a single atomic transaction. Keep #430's
bounded same-intent retry followed by **error only** on unresolved outcome;
do not invent an unconditional owner-revoke/forced-unmount/global reply
permit/journal/conditional compensation framework to claim otherwise.
**No advisory-lock failure can trigger FUSE umount or session exit**:
Redis errors, exhausted ambiguous outcomes, expired Lock Owner Lease,
`ENOLCK`, timeout of an individual Redis attempt and signal cancellation
return errors for the affected lock operation, not Mount teardown. The
Mount remains active with ordinary I/O independent of lock health.
Mount teardown is independent from the individual lock-error path
and is never initiated by a lock failure. While waiting, the *application syscall* remains blocked on the
kernel FUSE reply, but the daemon's request Fiber sleeps on a
`utils::FiberBaton` until the Mount scheduler selects it for a new Redis
attempt based on a **read-only** availability probe or individual cancellation
wakes it. Only that original Fiber then retries the existing atomic
grant. No FUSE dispatcher or Redis worker is pinned for the wait.
Distinguish normal interrupt semantics from this explicit failure-limit
contract. Tests must cover early/during/late interrupt, reply ownership,
failed reply and normal teardown; only #473 has proven remote capability
activation. #474 may activate only after tested traditional POSIX semantics and
unsupported-OFD risk characterization.
A cross-Mount deadlock detector and strict
lock-order fairness are not implicitly provided by retry loops.

### Design sign-off / test matrix

#### #428 Linux/libfuse transport verification

The lock authority must consume a **mount-scoped opaque kernel owner**, not
derive owner identity from PID, `fh`, or a process table. The selected libfuse
low-level API (`fuse_lowlevel_ops`) transports `getlk(fi, flock)`,
`setlk(fi, flock, sleep)`, `flock(fi, op)`, `flush(fi)` and `release(fi)`.
`fuse_file_info.lock_owner` is documented for locking operations and flush;
`flock_release` is **only** meaningful on release and guarantees a valid
`lock_owner` when set. `fuse_reply_lock` is the correct GETLK response, not
`fuse_reply_err(req, 0)`. `fuse_req_interrupt_func` synchronously invokes its
callback if the interrupt preceded registration; code must not access a
request after a synchronous callback may have consumed it.

The kernel-side logic in Linux `fs/fuse/file.c` uses `fuse_lock_owner_id` to
translate kernel owner identity and sends remote GETLK/SETLK/SETLKW when remote
POSIX locking is negotiated. It routes remote flock through its separate
flock path and indicates flock release on final close. A local mount/session
incarnation remains necessary even if two kernel-owner numbers happen to
match across mounts. Unlink/reopen paths must not invent lock ownership from
the per-open `fh`. Kernel-local fallback when remote flags are absent is
**not** evidence of cross-mount lock authority.

**OFD integration blocker (transport verified):** Linux `fs/locks.c` converts
`F_OFD_SETLK`/`F_OFD_SETLKW` into the ordinary `F_SETLK`/`F_SETLKW` internal
commands, retains `FL_OFDLCK`, and changes the owner to the open file
description. The FUSE lock wire input exposes only `FUSE_LK_FLOCK` as a
lock-class flag, not `FL_OFDLCK`; libfuse routes both OFD and traditional
record locks to the same low-level `setlk` callback without an explicit lock
kind. The target CI observer **confirmed both** `F_OFD_GETLK` and `F_OFD_SETLK`
arrive through ordinary low-level `getlk`/`setlk`, using the same opaque OFD
owner for both operations. On its final close, `FLUSH` carried the traditional
process lock owner instead; `RELEASE` carried `flock_release=0` and no OFD
owner. Therefore treating an OFD `setlk` grant as a traditional POSIX grant
would not remove it at the right time. #474 confirms the missing
POSIX-versus-OFD discriminator and OFD final-close owner, but a JuiceFS
source comparison superseded the conclusion that traditional POSIX must
remain disabled indefinitely. The #474 real two-Mount RED/GREEN tests
have now validated the supported traditional POSIX path, and PR #494
enables `FUSE_CAP_POSIX_LOCKS`. That does **not** supply OFD request-class
discrimination or full OFD lifecycle guarantees. Keep OFD unsupported and
do not reinterpret its behavior as supported or implement OFD.

**One-time investigation method, not a permanent CI gate:** a standalone
low-level FUSE observer and Python driver exercised Linux/libfuse directly,
without running SwordFS. The investigation source was deliberately **not
merged** as ongoing repository test infrastructure; the experimental revision
is recorded in [commit 47290c8](https://github.com/SwordInfra/SwordFS/commit/47290c87f9fc9f69e4a8ad7d700b1851eb3e8b0a).
The observer unconditionally granted SETLK and returned `F_UNLCK` for GETLK:
**its success establishes transport behavior, never distributed lock
correctness**. Its interrupt callback signaled a worker, which unregistered
the callback after its return and then replied once.

**Captured transport matrix:** [focused job in run 37791689635](https://github.com/SwordInfra/SwordFS/actions/runs/37791689635)
(and [successful full run 37792896892](https://github.com/SwordInfra/SwordFS/actions/runs/37792896892)),
artifact `fuse-lock-transport-evidence` (`fuse-lock-evidence.json`), Linux
`6.17.0-1022-azure`, libfuse `3.18.2`, x86_64. All **26/26 transport
assertions passed**, including OFD final-close identity checks and both
interrupt reply outcomes. GitHub artifacts have limited retention; this
matrix and its caveats are the durable conclusions.
The following are observations from that artifact, not inferred POSIX lock
authority behavior:

| Probe | Actual transport evidence |
| --- | --- |
| Independent `open`, `F_SETLK`/`F_GETLK`, unrelated close | `fh=100` vs `101`; both locks had owner `13572322902548544518`; closing `101` sent `FLUSH` with that owner and then `RELEASE` |
| `dup`, close, last close | Duplicated fd kept `fh=100`; intermediate close sent `FLUSH` without `RELEASE`; last close sent `RELEASE` |
| Fork/exec with inherited fd | Child used inherited `fh=100`, but POSIX owner `5227323326283428974`, different from parent; separate child `FLUSH` occurred |
| `flock`, fork/exec, dup, last close | `flock` owner `15731337448195210765` was inherited; intermediate close sent only `FLUSH`; final `RELEASE` sent `flock_release=1` and the same flock owner |
| Separate mounts | Both emitted `setlk`, but different opaque owner values for the same calling process; never globally compare raw owner IDs without session identity |
| `F_GETLK` reply | `fuse_reply_lock` returned `0`; syscall returned `F_UNLCK` |
| `F_OFD_GETLK`/`F_OFD_SETLK` | Both **succeeded** and reached ordinary callbacks, with the same OFD owner `15697360896097286241`; final `FLUSH` used POSIX owner, `RELEASE` did not identify that OFD owner |
| Disable remote-lock capabilities | `F_SETLK`, `F_GETLK` and flock completed locally with **no** locking callbacks (but `FLUSH`/`RELEASE` still occurred) |
| Blocking `F_SETLKW` and signal | `setlk(sleep=1)` occurred; interrupt callback saw `fuse_req_interrupted(req)=1`; callback was unregistered before reply. Replying `EINTR` returned syscall `-1/EINTR`; alternatively replying success `0` **after** the signal returned syscall `0` on this target kernel. Both FUSE reply writes returned `0` |

These results are for **normal close**. Linux `fuse_flush()` may return before
issuing `FUSE_FLUSH` if writeback or mapping error checks fail, or if it
negotiated `FOPEN_NOFLUSH` under the relevant conditions. Consequently a
daemon cannot promise an unconditional remote POSIX any-close cleanup event
on **every** kernel close in error paths; #431 must explicitly resolve the
remaining lifetime bound and recovery strategy, not silently claim it gets
all exceptional close notifications.

**Source-backed, not yet completely runtime-proven:** the libfuse API
guarantees synchronous callback delivery when an interrupt arrives before
`fuse_req_interrupt_func` registration, and permits `func=NULL` to unregister.
In pinned libfuse **3.18.2**, `fuse_req_interrupt_func` takes the request
mutex, installs the callback, calls it inline when `req->interrupted` is
already set, and only then unlocks. **Do not call a consuming `fuse_reply_*`
from that interrupt callback**: the reply may free/destroy the request while
the registration path still holds its mutex. A notification-only callback,
followed by unregister and reply in an independent owner, avoids this trap.
The same libfuse source shows `send_reply_iov()` calls `fuse_free_req()`
**even when the device reply write fails**. A negative reply-write return
is a failure signal, not permission to retry with the now-consumed `req`.
The request fiber must own registration, response, unregister and callback
state lifetime; it must not assume its own successful reply write is an
atomic commit with the backend or that userspace necessarily observed success.
The probe demonstrated two signal/reply interleavings, **not** a proof
that every possible race is harmless, a successful `fuse_reply_*` proves the
client consumed success, or a failed reply is recoverable. Failed reply-write
recovery, ambiguous Redis grant, late cancel after grant, and exact rollback
of same-owner conversion remain mandatory implementation gates for #430/#433;
the kernel FUSE reply is not atomically coupled to backend authority. No
unsafe fabricated reply-write failure is counted as verification evidence.

Design consequence for #431–#433: carry the exact copied `fi` owner/release
fields into the asynchronous fiber, distinguish POSIX any-close from flock
last-close, and make a single request fiber own reply/teardown. The
investigation probe's immediate interrupt reply is **not** a proposed
production cancellation policy: the production waiter must first resolve
whether an atomic grant committed, and handle lost reply, successful reply
and late interrupt races without leaving a ghost lock. Unregister and destroy
interrupt callback state only after establishing callback/request lifetime
safety. Do not enable remote capabilities under #428.

Source references: [libfuse low-level API](https://libfuse.github.io/doxygen/fuse__lowlevel_8h.html),
[libfuse 3.18.2 request and interrupt implementation](https://github.com/libfuse/libfuse/blob/fuse-3.18.2/lib/fuse_lowlevel.c),
`/usr/include/fuse3/fuse_common.h` for the pinned libfuse version, and
[Linux FUSE file operations](https://code.googlesource.com/linux/torvalds/linux/+/3cb12d27ff655e57e8efe3486dca2a22f4e30578/fs/fuse/file.c),
[Linux `fcntl` lock translation](https://code.googlesource.com/linux/torvalds/linux/+/c9049984f0e470af865c497c7f785fe895e5da9c/fs/locks.c),
and [FUSE wire ABI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/fuse.h).

The #428 transport investigation above satisfies this source/probe gate for
the recorded kernel and libfuse version; its findings do **not** establish
the backend lock authority, exceptional close cleanup, or every cancellation
race. Revalidate on materially different target kernel/libfuse versions
before claiming the same transport contract. The probe did **not** enable
unsupported lock classes in SwordFS.

The implemented #429 Redis owner-lifecycle design uses validated
`WATCH`/`GET`/`MULTI`/`INCRBY`/`EXEC`, `SET NX PX`, `PEXPIRE XX`, and `DEL`,
with default 60-second owner TTL, 10-second renew cadence, and 15-second
conservative self-stop reserve. #430 implements the independent Redis lock
grant authority with bounded ambiguous-EXEC retry then error-only; #431/#432
implement the nonblocking FUSE adapters. #471 must document and test bounded
waiter resources, **standard flock conversion behavior**, voluntary-unmount
termination and failure responses. Do not mandate a global reply-delivery
or lock-conversion compensation protocol to promise atomicity that does not
exist between Redis and the kernel. Protect the following
with Redis tests and end-to-end / two-mount tests: lock range
split/merge, read/read and read/write, owner upgrades, `GETLK` PID
limitations, POSIX any-close, flock last-close, `LOCK_NB`, wait and wake,
missed wakeup, cancel-before/after grant, ambiguous commit, stale owner
cleanup, Redis outage/restart, and reply-delivery failure. Tests must await
semantic completion, not use sleep as proof of correctness.

`generic/131` is now classified as **supported**: its raw upstream locktest
result was PASS on CI 38880840152 with remote POSIX capability negotiated,
and the independent real two-Mount E2E tests establish Redis-backed conflict,
close and wait behavior that a local-only generic/131 PASS would not prove.
`generic/478` exercises unsupported OFD `dup/clone/close` semantics and is
**DEFERRED_CI**, not counted as supported or a passing test. `generic/504`
remains upstream-not-applicable to Redis flock due to `/proc/locks`
introspection. The #473 two-Mount E2E separately proves remote BSD flock.
Neither a callback's existence nor a kernel-local-only passing test would
justify advertising remote lock support without the real Mount evidence.

References: Linux `fcntl_locking(2)` and `flock(2)` man pages; Linux
`fs/fuse/file.c` for lock/flush/release; libfuse low-level lock callbacks and
`fuse_req_interrupt_func`; JuiceFS `pkg/meta/redis.go` lock/session keys
and `pkg/meta/interface.go` for comparable lock operations. JuiceFS is a
reference, not proof that TTL alone fences a partitioned client.

## Anonymous temporary files (`O_TMPFILE`)

SwordFS implements Linux `O_TMPFILE` by composing the ordinary namespace and
open-file lifecycles instead of introducing a hidden-dentry or special-inode
state. The low-level FUSE tmpfile callback creates a regular file under a
reserved collision-resistant `.swordfs-tmp-*` name, establishes a normal
`FileHandle`, and then unlinks that generated name before replying to the
kernel. Generated-name `EEXIST` races are retried with a fresh name and never
overwrite an existing entry.

The ordering is deliberate. Establishing the normal `FileHandle` first takes
the inode's existing local open reference, so the subsequent last-link unlink
publishes an ordinary durable orphan candidate while the open-file fence keeps
the background reclaimer from preparing it. The successful FUSE reply returns
the same regular inode and handle with `nlink == 0`; normal read, write, flush,
setattr and link operations therefore need no tmpfile-specific branches. A
later hard link uses the existing metadata `Link` transition to revive the
orphan atomically, while closing an unlinked tmpfile simply releases the last
open reference and lets the existing orphan/reclaim path remove it.

The temporary name is briefly visible, so another actor can rename the
created inode away and replace that name before internal unlink. To avoid
deleting an unrelated replacement, tmpfile removal and its failure cleanup
pass the created `InodeID` as an expected-identity precondition to metadata
Unlink. Redis compares the current directory entry to this identity
atomically with deletion. A replaced name fails without being removed.
Ordinary user unlink has no such precondition. This guard does not remove the
accepted brief visibility of the temporary name.

This implementation accepts one bounded semantic approximation: the generated
name is briefly visible between metadata create and unlink. A crash in that
window can leave a reserved-name regular file behind rather than an anonymous
inode. That residue is intentionally ordinary namespace state and requires no
backend-specific recovery format. Once unlink has committed, crash recovery is
the normal durable orphan/reclaimer lifecycle.

Failure ownership follows the same ordinary primitives. A handle-creation
failure best-effort unlinks the created name. An unlink failure releases the
local handle before retrying cleanup, and the original unlink error remains the
syscall result. After unlink succeeds, failure to deliver `fuse_reply_create`
rolls back both the provisional FUSE lookup reference and the local file
handle; the already-committed orphan remains owned by normal background
reclamation rather than being synchronously deleted by the reply path.

Large tmpfile workloads can publish many orphan candidates at once. Orphan
preparation therefore runs in resumable bounded batches. Redis metadata
keeps an HSCAN page/cursor continuation across worker batch boundaries. The worker can abort an in-flight candidate
walk promptly during unmount and self-wakes to continue a non-terminal batch,
so a stress-created orphan backlog cannot make daemon shutdown wait for a full
queue scan.

## Model

The upstream revision is pinned in `conformance/pjdfstest/version.env`. A run
executes every relevant `tests/**/*.t` script against one privileged SwordFS
FUSE mount and preserves each script's TAP output. Assertions are identified as
`tests/<category>/<file>.t#<n>`; this avoids broad file-level skips and makes the
baseline stable at a pinned upstream revision.

Two project-owned baseline files interpret those observations:

- `supported.txt` lists assertions SwordFS claims to support. They must pass.
- `known-gaps.tsv` lists expected failures with an explicit category, linked
  Issue, and reason. Supported assertions and known gaps are mutually
  exclusive.

Both files use exact assertion selectors. A selector may compact adjacent IDs,
for example `tests/open/00.t#1-4,7,9-12`; ranges expand to those exact assertion
IDs. Wildcards and whole-file exclusions are deliberately unsupported, so a
passing assertion cannot be hidden by a broad expected-failure rule.

The classifier applies this state model:

```text
supported + PASS       -> PASS
supported + FAIL       -> REGRESSION (blocking)

known gap + FAIL       -> KNOWN_* (reported, not hidden)
known gap + PASS       -> XPASS (blocking until baseline is tightened)

unclassified + PASS    -> UNCLASSIFIED_PASS (blocking baseline debt)
unclassified + FAIL    -> UNEXPECTED_FAIL (blocking)

upstream SKIP/TODO     -> UPSTREAM_NOT_APPLICABLE
baseline + SKIP/TODO   -> BASELINE_NOT_APPLICABLE (blocking)
malformed/incomplete TAP -> INFRASTRUCTURE (blocking)
```

Treating an unclassified pass as baseline debt is deliberate: the supported
set remains explicit and monotonic instead of allowing newly working behavior
to exist outside the regression contract.

Known-gap categories are `known_unsupported`, `known_semantic_defect`, and
`environment`. Environment entries are reserved for reproducible limitations
of the supported runner rather than ordinary harness failures. Build, mount,
dependency, malformed-TAP, timeout, and similar harness failures are
infrastructure failures and block the job.

## Execution and CI

`scripts/conformance/pjdfstest/run.sh` owns the privileged execution environment. It:

1. checks out the exact upstream revision;
2. builds the upstream `pjdfstest` helper;
3. starts the same Redis/MinIO services used by SwordFS E2E tests;
4. formats and mounts a dedicated SwordFS volume with `allow_other` so
   pjdfstest assertions that change effective UID/GID can reach FUSE;
5. runs every upstream test script in an isolated directory on that mount;
6. preserves raw TAP, per-script exit status, SwordFS logs, and environment
   metadata; and
7. unmounts SwordFS and tears down dependencies even on failure.

The GitHub Actions conformance job builds SwordFS in Release mode and runs the
harness as root. `scripts/conformance/pjdfstest/classify.py` then creates `result.json` and
`report.md`. Strict classification is the regression gate; raw upstream exit
status is not used as a proxy for SwordFS support.

The normal E2E job remains separate. Conformance has different privilege,
baseline, reporting, and failure semantics and must not be reduced to another
GTest pass/fail bit.

## Metrics

The report exposes three different percentages:

```text
overall support = PASS /
  (PASS + KNOWN_UNSUPPORTED + KNOWN_SEMANTIC_DEFECT + blocking semantic fail)

supported regression pass = supported PASS / all observed supported assertions

classified rate = classified relevant assertions / all relevant assertions
```

`environment` and `upstream_not_applicable` do not enter the overall-support
denominator. Supported regression pass and classified rate both target 100%.
Per-category support is reported independently so a large test family cannot
hide a weak semantic area.

## Baseline lifecycle

The initial infrastructure run is expected to expose unclassified assertions.
Use its generated bootstrap candidates to establish the first baseline:

- every observed unclassified pass becomes a supported assertion;
- failures are grouped by semantic root cause or unsupported capability;
- one focused GitHub Issue may own many failing assertion IDs;
- each failure is then recorded in `known-gaps.tsv` with that Issue and a
  concrete reason.

Do not create one Issue per assertion and do not use a broad file/category skip
to make the job green. If one failing prerequisite causes a test script to
abort before its declared TAP plan completes, fix or explicitly investigate
that root cause; incomplete TAP is not silently converted into semantic debt.

After the baseline exists, the supported set is monotonic. Removing an entry
because it regressed is prohibited unless SwordFS intentionally changes the
current semantic contract. PR CI compares `supported.txt` with the PR base and
rejects removals without label-based exceptions. Any intentional semantic
contract revision requires a separately reviewed explicit gate-policy change;
it cannot be authorized by adding a PR label. A known gap that starts passing is XPASS and must
be moved into the supported set in the same change.

When a root-cause Issue is resolved, re-evaluate **every selector linked to that
Issue**, not only assertions that became XPASS. A prerequisite failure can
create many downstream failures; after the prerequisite is fixed, a downstream
assertion may still fail for a different reason. Such assertions must be moved
to the newly demonstrated root cause instead of remaining hidden behind the
old Issue classification.

The same re-attribution rule applies even when no implementation change makes an
assertion pass. If raw TAP evidence shows that a failure is only a downstream
consequence of another unsupported prerequisite or defect, the baseline must
move that assertion to the actual prerequisite Issue immediately. A still-red
assertion is not evidence that its previous root-cause classification remains
valid.

Updating the pinned pjdfstest revision is an explicit reviewable change. Run
the new revision, inspect the baseline delta, classify added/changed assertions,
and only then update the baseline.

## Artifacts and public status

Each CI run uploads raw TAP, classified JSON, the Markdown report, SwordFS
runtime logs, and environment metadata. PR runs are previews only and cannot
change canonical project status.

Authoritative `main` runs publish the latest report and machine-readable result
to the isolated `pjdfstest-status` branch. The top-level README links to that
stable branch. Publication also keeps `history.json` and a compact historical
trend in `status.md`; it never creates generated commits on `main` or recursively
triggers source CI.

`history.json` retains every authoritative run for machine-readable audit and
diagnostics. The human-readable historical trend is intentionally sparser: it
adds a point only when `Overall support`, `Supported gate`, or `Classified`
changes from the preceding trend point, whether the value increases or
decreases. Runs that only repeat the same metrics, including infrastructure-only
status changes without classified metrics, do not add trend rows.

If the latest main run has semantic blockers, the status page records that
failed result. If the conformance job fails before a classified report exists,
publication records an infrastructure failure rather than leaving an older
healthy status presented as current.
