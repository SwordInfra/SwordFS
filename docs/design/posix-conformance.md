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
rename. Memory and persistent metadata backends share the same limit and
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

This section defines the proposed **first-stage** cross-mount locking contract.
It is not a claim of implemented support: current `getlk`/`setlk` callbacks
return `ENOSYS` and `flock` returns `NotSupported`. FUSE remote-lock feature
flags must not be enabled before authority, lifecycle, and transport tests
prove their semantics.

### Implementation decomposition and handoff

Parent #379 owns this cross-class architecture contract, integration,
conformance classification and final closeout; its children own bounded
design, implementation, test and review evidence. Use the repository's
**native GitHub sub-issue hierarchy**, not an unrelated backlog of PRs:

| Issue | Deliverable | Depends on |
| --- | --- | --- |
| #428 | Verify Linux/libfuse owners, close and interrupt behavior; classify OFD | — |
| #429 | Minimal mount-owner lease identity, liveness and fail-closed loss protocol | #428 for finalized transport identity |
| #430 | Memory/Redis atomic lock authority, expired-owner filtering, optional lazy GC and uncertainty recovery | #428, #429 |
| #431 | Traditional POSIX `getlk`/`setlk` and any-close release | #428–#430 |
| #432 | BSD `flock` and open-file-description last-close release | #428–#430 |
| #433 | Interruptible waits, cancellation, cross-mount integration and activation | #431, #432 (and the shared prerequisites) |

The actual changes may be reviewed separately as each child reaches its own
design-readiness checkpoint. The parent remains open until integration on
both backends, relevant fstests classification, post-merge main CI and review
are reconciled. #429 establishes **owner lease validity**, not a general
Redis metadata session or a lock cleanup service. #430 owns actual lock
records, atomic owner-validity checks and optional lazy reclamation. No
intermediate child permits enabling incomplete remote-lock support or
claiming OFD/delegation compatibility. The known
conversion/reply-delivery ambiguity is a mandatory #430 authority decision,
then an integration regression case in #433; do not leave it as an orphan
parent-only concern.

### Supported classes and transport

**First-stage product contract (authority-level distributed advisory locks):**

| Guarantee | Required invariant |
| --- | --- |
| **G1 Atomic Grant** | At each lock-authority serialization point, no two valid owners have mutually incompatible grants for the same inode, range and class. Compatible shared grants are allowed. |
| **G2 Owner Validity** | Missing/expired/revoked generations cannot obtain new grants; before a successful lock reply, the client checks its matching generation and shorter conservative local lease cutoff. |
| **G3 Owner Isolation** | Retry, unlock and cleanup belonging to an earlier generation never modify a later generation's locks; unknown outcomes use stable operation identities. |
| **G4 Bounded Logical Recovery** | Orphaned owner locks cease conflicting immediately after authoritative lease expiry/revoke, without waiting for physical GC. Explicit `umount` revokes the generation in O(1) Redis key operations rather than enumerating grants; later inode lock operations lazily reclaim invalid records. |

**Deliberate limitation:** these are guarantees about the *authority's
authorization state*, under its stated single-primary continuity and clock
assumptions—not a guarantee that two arbitrary application critical sections
never overlap during lease loss. An earlier `fcntl`/`flock` success cannot
be retracted; the application may not observe lease expiry and might continue
ordinary I/O even after another owner legitimately acquires the replacement
lock. No generic asynchronous lost-lock notification can guarantee the
application stops. Do **not** add mandatory read/write fencing, a generic
client Session or lock-lease notification framework, per-I/O Redis probes,
or broad reply-permit machinery to claim this impossible property.
Ordinary filesystem I/O remains available on lock-lease failure; only the
old lock generation loses authorization. Client-side pre-reply local-time
validation narrows but does not eliminate the last check-to-send race.
Any stronger data/resource fencing is a **separate product capability**,
not a dependency of #429–#433. See epic #379 and child #429/#430 for
corresponding acceptance tests.

The intended first stage supports traditional **process-associated POSIX
record locks** (`F_GETLK`, `F_SETLK`, `F_SETLKW`) and **BSD `flock`**
(`LOCK_SH`, `LOCK_EX`, `LOCK_UN`, `LOCK_NB`). They are advisory: an application
that does not lock may still read/write. Locks on separate mounts of one
Redis-backed volume must observe the same conflicts. The Memory backend
implements the equivalent contract within its one in-process authority;
Memory is not a cross-process distributed metadata service.

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

**Explicitly excluded:** OFD record locks (`F_OFD_*`) and file
leases/delegations (`F_SETLEASE`, `F_SETDELEG`). OFD is not BSD `flock`; the
selected low-level FUSE `getlk`/`setlk` callback does not expose an explicit
OFD-versus-process lock-kind argument. Do not claim OFD support until
verified on the target kernel/libfuse. `generic/478` exercises mixed OFD
and POSIX behavior and cannot be promoted solely because POSIX locking
passes. `generic/786` and `generic/787` require separate delegation support.
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
native **`WATCH`/`GET`/`MULTI`/`INCR`/`EXEC`**, checking that the
*old* counter is nonnegative and below signed-64-bit overflow **before**
committing the increment, then creates its lease using `SET NX PX`. Counter
allocation and lease creation are **two separate transactions**: losing an allocated generation or
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

- One bounded **owner lease record** per mount incarnation: identity and a
  Redis TTL (Memory uses its equivalent local authority clock). A separate
  durable `ACTIVE/REVOKED` flag is **not required**: the existence and TTL of
  the never-reused lease key provide the authoritative validity predicate.
  An absent, revoked or
  expired record is **invalid**. Renew never creates a missing lease and cannot
  change its incarnation. An expired identity cannot re-register.
- Per-inode POSIX record locks: canonical owner-keyed, nonoverlapping
  `[start,end]` intervals with read/write mode and optional diagnostic PID.
  End-of-file (`l_len == 0`) has an explicit infinity sentinel; validate
  range normalization and overflow, not physical file size.
- Per-inode BSD flocks: whole-file shared/exclusive owner records.
- **No mandatory owner-to-inode reverse index** for `umount`. The
  generation-scoped lease key is the sole owner-liveness authority; deleting
  that key invalidates **all** grants from that generation regardless of
  how many inodes/ranges it locked or whether individual grant replies were
  delivered. Per-inode lock records remain the grant authority and are
  reclaimed lazily, only after verifying their owning lease is invalid.
  A reverse index is allowed only if later justified for *independent*
  retention/maintenance requirements, never as an unmount prerequisite.
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

1. `RegisterOwner` allocates a new per-volume numeric generation under
   `WATCH owner_generation_key`: `GET` the initialized counter, parse the
   full Redis signed-64-bit integer strictly and require
   `0 <= old < INT64_MAX`, then queue `INCR` and `EXEC`. The WATCH guards
   both concurrent updates and deletion/expiry of the counter. If EXEC
   succeeds, `generation = old + 1`; on retry or ambiguous EXEC result,
   discard this candidate and start again with a new WATCH/read, never
   assume an unconfirmed generation. A malformed/negative counter fails
   **without modifying Redis**, unlike the unsafe pattern `INCR` followed
   by client-side unsigned post-validation. The existing
   `RedisMetaClient::Transact`/`RedisKvTxn::{Get,IncrBy}` supports the native
   optimistic steps, with `OutcomeUnknown` treated as unconfirmed. The
   existing `RedisMetaClient::IncrNonNegative` is Lua-based and **not
   required**. **Initialize `owner_generation=0` in the same Redis
   transaction that creates the volume's `format` record**, before any
   owner registration. The counter is persistent and never TTL'd. At
   mount/registration time, an absent, malformed or negative counter is
   an integrity failure for the **lock service**, never a reason to create
   or reset it. This beta-stage contract has **no compatibility migration,
   legacy-volume bootstrap or auxiliary schema marker**. Loss or rollback
   of acknowledged generation history is unsupported.
   It issues a **single** `SET owner_lease:{generation}
   <incarnation_nonce> NX PX <lease_ttl_ms>` for that fresh key. `SET` must return
   confirmed `OK` before the mount can acquire locks; a collision is an
   authority integrity failure. Lost `INCR`/`SET` responses cause a **fresh
   generation allocation**, never another `SET` against the abandoned key;
   the client must not automatically retry that SET on connection recovery.
   No client-supplied `register(old_incarnation)` endpoint exists; abandoned
   lease keys expire normally. A lost/rolled-back generation counter is
   outside the supported authority-history model and requires fail-closed.

2. `RenewOwner(generation)` uses native `PEXPIRE owner_lease:{generation}
   <lease_ttl_ms> XX` (Redis 7.x). `XX` requires an existing TTL and does **not**
   create a missing lease; expiration or explicit revocation therefore
   cannot be undone by a late renewal. Return `1` confirms extension, `0`
   means the lock epoch is invalid, and a missing reply is **unknown**, not success. A
   renewal queued before local cutoff may complete late: it cannot revive a
   Redis-expired key, and the lock epoch remains unavailable if confirmation
   arrives after its conservative deadline. Serialize renewal and lock-epoch
   invalidation locally; do not issue new renewals for an invalid epoch.
3. `RevokeOwner(generation)` first disables successful lock grants/replies for
   the exact epoch, then issues
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
5. Physical reclamation of inert lock records is **opportunistic**,
   identity-conditional, idempotent and not a prerequisite for a new grant.
   #430 must bound **per-operation** work (including incremental lazy GC)
   and document cold-key storage accumulation and an *optional* separate
   maintenance policy if aggregate retained storage needs a hard cap. Do
   not introduce a background full-volume scan or owner index merely to
   make explicit `umount` fast; its correctness depends on lease revocation,
   not physical removal.

#### Redis metadata layout and lock-operation protocol (#429 → #430)

**Candidate schema for design review, not implemented or finalized #430
serialization.** Reuse `RedisKey`'s existing per-volume namespace
`{<redis_db>:<volume>}:` for every key. Avoid storing per-mount locks in
ordinary inode/chunk data metadata or treating Redis connections as owners.

| Redis key suffix | Type | Payload | TTL and owner |
| --- | --- | --- | --- |
| `owner_generation` | String integer | Persistent signed-64-bit monotonic generation counter, initialized to `0` **atomically with volume format** | **No TTL**, #429; missing on registration is an integrity error, validated with watched read before increment |
| `owner_lease:<generation>` | String | Mount incarnation nonce/identity evidence | **Configured** `lease_ttl`, #429. Existence with `PTTL > 0` means valid |
| `lock:posix:<ino>` | Hash | Owner field `(generation,kernel_lock_owner)` → encoded canonical nonoverlapping read/write byte-range set, optional diagnostic PID | **No per-lock TTL**, #430 |
| `lock:flock:<ino>` | Hash | Owner field `(generation,kernel_lock_owner)` → encoded whole-file shared/exclusive state | **No per-lock TTL**, #430 |
| `lock:version:<ino>` | String integer | Lock-change sequence for waiter recheck/notifications | **No lease TTL**, #430/#433 |
| `lock:operation:<generation>` | Hash or equivalent bounded journal | Stable `(operation_id, owner, inode, class, outcome/revision)` for `OutcomeUnknown` replay | Retention **distinct from lease TTL**; concrete safe pruning protocol belongs to #430 |

This layout is a **proposal** for explaining how #429 and #430 compose. It
does not yet fix exact binary serialization, field names, revision encoding,
per-inode class sequence sharing or operation-journal retention. Those need
#430 proof and acceptance before implementation. The deliberate omission of
`lock:owner:<generation>` is **not** a missing cleanup mechanism: the single
lease key already represents validity for every grant in that generation.
An expired owner may leave inert fields in per-inode Hashes; **do not** put
a TTL on each shared inode lock Hash or run `SMEMBERS`, `SSCAN`, `KEYS` or a
whole-volume sweep merely to complete `umount`. Writes that change grants,
inode versions or operation journals must remain one atomic authority
operation. In particular, a journal entry for an uncertain Redis commit
does not make the old generation valid after its lease has been revoked.

**Nonblocking POSIX grant / flock grant** (illustrative transaction):

1. FUSE supplies `ino`, `kernel_lock_owner`, class, mode and range; #429
   supplies the current `(volume,generation)` and checks its local
   `CLOCK_BOOTTIME` cutoff. Allocate a stable request/operation ID; do not
   create a second ID on an ambiguous Redis commit.
2. On one Redis transaction connection, `WATCH` the requesting lease key,
   appropriate inode lock Hash, operation journal and other keys **whose
   previously read state is relied on**. No owner-to-inode Set is read or
   updated for an unmount-related cleanup path.
   Read the relevant Hash and discover owner generations. Then `WATCH`
   **every generation whose lock influences the conflict calculation**
   before `PTTL` checking its lease, including the requester. The already
   watched inode Hash prevents an unseen concurrent new holder from being
   committed unnoticed. A requester with `PTTL <= 0` cannot receive a grant;
   holders with `PTTL <= 0` are ignored as stale. Redis 7 aborts `EXEC` if
   any watched valid lease expires or is renewed/revoked during this phase.
3. Normalize range/conversion and check conflicts against valid holders.
   For a real conflict return the correct `EAGAIN`/`EACCES`/`EWOULDBLOCK`
   without recording an unearned grant. Shared/shared is compatible; POSIX
   and flock are different conflict namespaces.
4. If grantable, queue `MULTI`, owner-conditional `HSET`/`HDEL` lock state,
   `INCR` of the change sequence,
   and an operation result/revision record. `EXEC` is the single authority
   linearization point. A WATCH abort restarts the **whole** read/check
   within a bounded retry budget; a post-EXEC `OutcomeUnknown` is reconciled
   with the **same** operation ID and journal, not blindly granted again.
   Redis MULTI is not rollback-on-runtime-error: prevalidate write types and
   treat command-level partial errors as uncertain, never automatic success.
5. **Immediately before a successful FUSE lock reply**, validate that the
   original local generation remains `LOCK_ACTIVE` and current
   `CLOCK_BOOTTIME < local_cutoff`. Otherwise reply with an error and let
   #430 reconcile the committed but unacknowledged grant. A check-to-send
   race remains; this is not strong application critical-section fencing.

**Query and release:** `GETLK` reads the same per-inode class Hash and
authoritative lease TTLs to report only conflicts from valid owners;
`F_UNLCK` modifies/splits a POSIX owner's ranges while preserving its other
ranges. The traditional POSIX any-close `FLUSH` releases all its ranges on
that inode; BSD `flock(LOCK_UN)` and the last `RELEASE` remove the specific
open-file-description owner's whole-file grant. Successful mutations update
the inode version and operation journal atomically.
Read-only query snapshots must not bypass the same validity rules, although
a queried lock can naturally change immediately after `GETLK` returns.

**Expiry, explicit unmount and lazy reclamation:** if an Owner lease expires
or is revoked, its String lease key disappears, while per-inode Hashes may
still contain records from the invalid generation. On **explicit `umount`**:

1. Stop new lock admissions, finish/cancel any admitted lock operation
   within a bounded local shutdown phase (ambiguous outcomes become inert
   to other owners **after owner invalidation**, so do not wait indefinitely
   to enumerate/reconcile every grant). Stop and **join** the heartbeat so no late
   renewal can extend the outgoing generation; never destroy its Redis
   client while a renewal may still run.
2. Issue `DEL owner_lease:<generation>` once (with bounded idempotent
   retry), after local lock quiescence. This is the **authority-side
   linearization point** for making every grant owned by that generation
   ineligible, regardless of quantity or lost replies. No lock Hash reads,
   reverse index, `HDEL` loop, `SSCAN` or per-inode remote cleanup is
   required for `umount`. Then perform the existing FUSE teardown.
3. If Redis is unavailable or the `DEL` reply is ambiguous, local shutdown
   still completes within its bound; **do not** claim immediate remote
   unlock. Once the last possible in-flight `PEXPIRE XX` has completed,
   Redis TTL bounds the remaining remote lifetime, assuming the declared
   clock/authority history. A future grant from another mount will treat
   it as valid until authoritative expiration if revocation was not
   committed. No old-generation retry can revive a deleted lease.

The shutdown gate must also prevent per-file `FLUSH`/`RELEASE` callbacks
generated **solely by FUSE teardown** from reintroducing an O(number of
open files/locks) Redis cleanup loop. Ordinary delivered close/unlock
while a mount is healthy still performs its normal POSIX/flock release;
once shutdown invalidation begins, the single owner-lease revoke is the
authoritative release for **all remaining** lock records. Do not confuse
an ordinary file-data flush with this lock-only shutdown optimization.

On the **next operation touching a particular inode**, read the inode's
lock Hash, identify generations recorded there and check each relevant
lease's `PTTL` in the same watched authority snapshot. Records whose lease
is missing/expired are **not conflicts**. For a modifying operation,
opportunistically queue exact-field `HDEL` of invalid records in its
`MULTI/EXEC`, alongside any new grant and version/journal update.
This makes the record set smaller on naturally active inodes without
requiring a special expiry or `umount` scan. A read-only `GETLK` may simply
ignore invalid records; physical GC can be deferred to a later write.
Watch the inode Hash before inspecting it, watch every lease whose state
is used for a live conflict, and recheck the requester lease. Once an old
generation is invalid it is never reused, so a stale cleanup cannot
delete a newer generation's differently keyed grant. If an inode field
is reused for the **same** generation/owner while it remains active,
conditional cleanup must also compare its revision/content; do not infer
that a late physical `HDEL` is harmless merely because it names an owner.

**Resource bounds:** the no-scan `umount` is independent of lock count
(O(1) Redis keys). An inode operation may still encounter many **cold
stale fields**. Read amplification, per-operation lazy delete batch size,
Redis memory use and inode-key retention are explicit #430 design/benchmark
gates. Bound cleanup work per touch and, if production requires a strict
global cap on never-revisited inode records, design a *separate optional*
garbage-collection policy. O(1) unmount and lazy reclamation alone do
**not** guarantee bounded total physical Redis storage.

**#430 representation choice for large inodes (still open):** compare the
baseline flat `lock:posix:<ino>` / `lock:flock:<ino>` Hashes (one field per
`(generation,kernel_owner)`) against **per-inode generation grouping**:
an inode-local generation Set plus one lock Hash per `(ino,class,generation)`.
Grouping can clear one stale generation's many process-owner fields with
an identity-checked `UNLINK` and an inode-local `SREM` when that inode is
next touched; it does **not** create any global Owner→inode index or add
unmount work. The tradeoff is substantially more Redis keys/transactions,
and both layouts can still accumulate many stale generations on a cold
inode. Benchmark normal shared read-lock fanout, range-heavy writers,
large numbers of dead generations and single-hot-inode cleanup before
choosing. `HGETALL`/`SMEMBERS` over unbounded stale state is **not** a
bounded-latency solution; a correct grant must check all potentially
conflicting *live* records or report an explicit bounded failure/retry,
never skip unseen records and claim the lock is free.

**Blocking operations (#433):** if a conditional grant conflicts, release
the Redis transaction, observe the inode change sequence, subscribe for
wakeup, recheck the sequence after subscription and retry only when woken or
after a bounded fallback interval. Notifications can be lost; they are an
optimization, not the authority. Interrupt/cancel cannot leave a committed
grant unacknowledged without exact operation reconciliation. The wait
interval and operation-journal GC are **not** the Mount Owner `lease_ttl`
and must be designed independently.

**Lease timing: initial normative defaults** (configuration bounds and
operational review are part of #429 TDD):

| Parameter | Default | Rationale / validity rule |
| --- | --- | --- |
| Owner lease TTL `lease_ttl` | **60 s** | Bounded orphan validity once the last renewal reaches the server; no lock-by-lock TTL. |
| Heartbeat period `renew_interval` | **10 s** | Dedicated lock-owner control worker, independent of application lock traffic; jitter/bounded retry must not postpone local cutoff. |
| Local safety reserve `safety_margin` | **15 s** | Stop treating old lock grants as valid by `CLOCK_BOOTTIME(send) + lease_ttl - safety_margin` after the **last confirmed** create/renew dispatch. Earlier than Redis TTL under explicitly bounded clock/network assumptions; ordinary FUSE mount remains. |
| Lease operation total deadline `request_timeout` | **5 s** | **End-to-end** deadline for each registration/renew/revoke attempt, including connection/pool wait and internal retries; do not mistake one Redis socket timeout for the whole deadline. |

**Configuration is part of the #429 design, not hard-coded time constants.**
Expose these four mount-local, startup-snapshot parameters on the SwordFS
`mount` subcommand (not `format`, not volume-wide persisted metadata, and not
Redis connection URL parameters):

```text
--lock-lease-ttl=60s
--lock-lease-renew-interval=10s
--lock-lease-safety-margin=15s
--lock-lease-request-timeout=5s
```

The options will be represented as `std::chrono::milliseconds` in a coherent
`LockLeaseConfig` value read from `config::ConfigCenter` on mount startup.
Do not introduce a global generic timeout service or duplicate Redis
transport settings. Use the existing CLI duration-validation conventions;
support explicit `ms`, `s` and `m` units and reject unrecognized units,
overflow, and non-positive values. The config snapshot is **immutable for
the lifetime of a lock generation**: no live TTL shortening, hot reload,
or renegotiation of an existing generation. Other mounts may use different
valid durations because each owner's TTL belongs to its *own* Redis lease
key; there is no single per-volume lease TTL.

**Initial validation policy** (design limits, subject to operational review):

```text
10 s <= lease_ttl <= 10 min
1 s <= renew_interval
1 s <= safety_margin < lease_ttl
1 s <= request_timeout < renew_interval
renew_interval + request_timeout + safety_margin < lease_ttl
renew_interval <= (lease_ttl - safety_margin) / 2
request_timeout <= safety_margin
```

Reject invalid combinations at **mount configuration parsing**, before
creating any Owner generation/lease. The final two checks reserve more than
one practical renewal opportunity and prevent a single outstanding attempt
from consuming the entire local safety reserve; they do not magically
bound Redis clock jumps. The configured `safety_margin` must independently
cover the deployment's assumed maximum Redis clock acceleration, client
clock drift and dispatch/network uncertainty; the default 15 s is a
starting policy, **not** a universally verified fencing margin.

With the defaults, `local_cutoff = last_confirmed_send_boottime + 45 s`
because `60 s - 15 s = 45 s`. With a valid alternate configuration such as
`120s / 20s / 25s / 5s`, that same expression gives a 95-second local
cutoff. All `SET PX`, `PEXPIRE XX`, retry timers, metrics and tests must
use the configured values. The values are **not** independent from Redis
connection `connect_timeout`, `socket_timeout`, `pool_wait_timeout` or
retry policy: the owner-lease adapter must impose its own absolute
end-to-end `request_timeout` even when generic Redis transport defaults
would exceed it. Reuse the existing execution-domain facilities without
introducing a blocking heartbeat in a FUSE callback. When distributed
locking is disabled, these settings must not cause lease registration or
background Redis traffic. Normal file I/O must never pay a new Redis RTT
solely for advisory locking.

The client samples **Linux `CLOCK_BOOTTIME` at request dispatch**, not at
response arrival, and after a confirmed registration/renewal sets
`local_cutoff = send_boottime + lease_ttl - safety_margin`. A delayed success after the
previous cutoff must **not** rescue an invalid lock epoch; a success observed
before cutoff may extend confidence only up to its *own dispatch-based*
cutoff. `CLOCK_BOOTTIME` accounts for suspend, unlike an assumed
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
permitted before cutoff, since renewing the same key does not duplicate
grants. A late success after cutoff never reactivates the **old lock epoch**.
Redis return `0`, ambiguous primary history, or reaching cutoff moves the
**lock subsystem**, not the FUSE mount, into `LOCKS_UNAVAILABLE`. Any
currently held lock grants of that epoch are invalid locally, and are no
longer considered active by the authority once the corresponding Redis lease
is expired/revoked. A disconnected client cannot synchronously delete Redis
keys: do not claim that local invalidation constitutes successful remote
unlock. When Redis is available, best-effort exact-generation revoke/cleanup
can make lock authority release prompt; otherwise the remaining Redis TTL
bounds logical orphan validity. An old lock epoch cannot silently renew or
resume. Existing file handles and ordinary non-lock FUSE operations stay
mounted; their own metadata/IO errors are independent of the lock lease.

**Two separate lifecycles:** the FUSE mount stays mounted during lock-lease
failure. The lock subsystem has `UNREGISTERED -> ACTIVE -> LOCKS_UNAVAILABLE`
for one owner generation. It may eventually start a **fresh** lock epoch
while keeping the FUSE mount, but **only after** proving the kernel's
already-acknowledged POSIX/flock lock state cannot be confused with the new
generation, all earlier requests are fenced, and applications must reacquire
locks explicitly. Until that recovery contract is implemented and tested,
lock calls return an appropriate service/error status (`EIO` for unknown
backend outcome; `ENOLCK` for unavailable lock service; `EAGAIN`/`EACCES`
only for real contention); normal reads/writes/stat/open/close continue.
Do not silently regrant, renew or pretend existing POSIX advisory locks
have been preserved. FUSE has no general callback that asynchronously
informs an application its previously successful `fcntl(F_SETLK)` lock has
been revoked; an application already in its critical section may continue.
That is a documented advisory-lock safety limit, not a reason to forcibly
`umount` the filesystem.

**Reply-time local deadline is the primary guard:** after #430 has
committed a lock grant, immediately before the successful low-level FUSE
reply, sample `CLOCK_BOOTTIME` again and atomically read the local lock-epoch
state/generation. Success requires `epoch == request_epoch`,
`state == ACTIVE`, and
`now < last_confirmed_send_boottime + lease_ttl - safety_margin` (the
default configuration yields 45 s; it is not a fixed compiled-in value).
If any condition fails, reply with an error; **do not** silently report
the Redis grant as an application success. #430 must reconcile a Redis grant
whose reply is rejected or fails, using its exact operation/owner identity.
This check is cheap and does not contact Redis, and is more useful here than
making every FUSE reply hold a heavyweight mount-wide permit. Checking
right before reply still has a small check-to-send race and cannot retract
a reply already being sent. Do not promise exact cutoff atomicity without
an additional proven serialization protocol, and do not apply the guard
blindly to every non-lock I/O reply.

**Unmount is different:** an explicit, orderly `umount` initiates
quiescence of **new lock requests**, lets admitted lock operations reach
a safe outcome and joins/stops the Owner Lease renewal worker. It then
issues one exact-generation `DEL owner_lease:<generation>` with a bounded
retry budget before destroying its Redis client and FUSE session. Successful
DEL is the **O(1) logical release of every lock held by that generation**,
including grants whose Redis commit succeeded but FUSE reply was lost.
There is **no per-inode enumeration, reverse index or eager Hash deletion**
on this path. If Redis is unreachable or the DEL outcome is ambiguous,
record the result and allow TTL to invalidate the generation eventually;
do not hang umount waiting for Redis. On future operations accessing the
same inode, #430 ignores expired-generation fields before checking lock
conflicts and opportunistically removes them, conditional on the exact
identity/revision. This is logical invalidation, **not a guarantee of
immediate physical cleanup** of cold inode records. Normal POSIX/flock
close/unlock while a healthy mount runs still releases individual locks
as required by their semantics. No lock cleanup may remove a newer owner's
grant, and no generic Redis metadata Session service is needed.

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
Native WATCH/GET/MULTI/INCR/EXEC, `SET NX PX`, `PEXPIRE XX`, `DEL` provide
the required registration validation and lease operations without a script;
two-phase registration is safe under the
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

**Focused Redis evidence after #435 merge (2026-10-09):** a standalone
disposable Redis `7.4.11` container, accessed using two raw Redis protocol
connections, established four native-command facts without compiling or
mounting SwordFS:

| Experiment | Observed result | What it does **not** prove |
| --- | --- | --- |
| `INCR`, one-shot `SET NX PX`, `PEXPIRE XX` after key expiration | Registration succeeded once; a duplicate `SET NX` failed; renewal returned `0` after expiry rather than recreating the key | Correctness of the SwordFS owner adapter or detection of Redis history rollback |
| `WATCH` live owner + inode lock key; wait for owner TTL to expire; `MULTI/SET/EXEC` | `EXEC` aborted and no lock key was written | Correctness when the real #430 transaction inspects many ranges and owners |
| `WATCH` live owner; another connection runs `PEXPIRE XX`; then `MULTI/SET/EXEC` | `EXEC` aborted on the concurrent renewal | Correctness and bounded retries under many frequent renewals |
| Delete old generation after registering the next generation | Old `DEL` could not delete the new-generation lease | Safe cleanup of actual per-inode lock records under #430 |

The probe used separate short (~0.1–1 s) test TTLs only to force expiration;
production defaults remain 60/10/15 s. The isolated container was stopped
and removed. This is **design validation of Redis semantics, not a #429
TDD GREEN or a full lock-authority correctness proof**; no permanent CI gate
or test harness is introduced for a one-off experiment. The #430 issue
retains the multi-holder, unknown-outcome, expiry and conflict-order proof.

#### #429 mount integration, failure transitions and TDD seams

The existing `cmd/Mount.cpp::Mount()` holds a `FuseSessionGuard` around
`fuse_session_new`, `fuse_session_mount` and `fuse_session_loop_mt`.
`fuse::MountInitContext` is stack-owned and provides the session pointer and
daemon-readiness fd to `VfsHookFactory::SwordFsInit`. The owner-lease worker
must **not** retain a borrowed `MountInitContext*` or a `fuse_session*` to
destroy or exit it upon lease failure. The worker owns **lock validity**;
only an independent, user-initiated unmount owns FUSE session teardown.
The worker itself must be stopped/joined as part of that ordinary unmount
before it can outlive its Redis/Volume dependencies.

| Phase | Required action / invariant |
| --- | --- |
| Disabled remote locking | No Redis lease registration, heartbeat or new metadata RTT; existing ordinary mount behavior remains unchanged. |
| Mount starting with remote locks enabled | On a thread-domain executor, allocate/confirm the owner lease *before* the FUSE INIT readiness success; start heartbeat under the same mount lifetime. On ambiguous allocation/registration, no inode lock is ever attributable to the abandoned generation. |
| `ACTIVE`, renewal transient error | Keep a single renewal in flight and cap total timeout/retries; a single timeout does not invalidate a still-live lease or disable ordinary I/O. |
| Redis `PEXPIRE XX` returns `0`, authority-history uncertainty, or local cutoff | Atomically mark that **lock epoch** unavailable, reject pending/new lock requests, and fail a success reply if the epoch/deadline no longer passes its immediate pre-reply check. **Do not exit, unmount, disconnect or kill FUSE.** Ordinary filesystem operations remain supported subject to independent backend failures. |
| Redis connectivity returns after lock-epoch loss | Do not resume the expired identity or implicitly restore old locks. A fresh lock epoch in the same FUSE mount requires a separately verified stale-kernel-lock/reacquire protocol; otherwise lock calls continue to error, but filesystem operations do not require remount. |
| Explicit normal `umount` | Stop admitting new locks, quiesce admitted lock work, stop/join heartbeat and `DEL` the exact generation's **lease key** before existing FuseSessionGuard teardown. Do **not** enumerate or physically delete all POSIX/flock records. Successful DEL invalidates all those grants at once; Redis failures use bounded best-effort/TTL fallback. |

**Minimal pre-reply safety check:** at every *lock-grant success reply*
load `CLOCK_BOOTTIME` and the atomic lock-epoch status immediately before
calling the relevant `fuse_reply_*`. Require the same generation that
started the Redis operation, `ACTIVE`, and `now < conservative_cutoff`.
If not, reply with a lock-service error and let #430 reconcile a Redis
grant that was committed but not acknowledged as success. Do not introduce
a generalized reply-permit counter over all FUSE requests solely for
lease invalidation. A concurrent state transition between this check and
the actual reply remains possible; the final check narrows rather than
eliminates the race. It is **not** a promise of strict temporal fencing of
already-running application code. A later implementation may justify a
lock-only reply serialization primitive if a stronger guarantee is required.

**Normal unmount and in-flight work are separate from lease loss:**
SwordFS `utils::FiberRuntime::Shutdown()` currently waits for pending
fibers to reach zero without a timeout, and `SwordFsDestroy` invokes it
before `FuseSessionGuard` destroys the libfuse session. #429 must not
turn an expired Redis lease into this shutdown path. On explicit `umount`,
the existing bounded-shutdown/hung-task behavior needs independent review:
record and drain **lock** tasks, resolve or cancel blocked lock waits,
stop the lease worker, and avoid destroying FUSE session objects still
referenced by requests. Lease revocation must not race a late successful
lock reply. A failed Redis cleanup does not justify keeping the whole
mount blocked indefinitely; logical invalidation by TTL remains the
fallback. Separate improvements to the generic fiber shutdown are not a
prerequisite for reporting lock-service failure while the mount remains.

**Authority continuity (supported vs unsupported):** the first stage assumes
one Redis primary with an unchanged acknowledged history; it does not
support transparent failover. A reconnect to the same endpoint is not by
itself proof that the primary is unchanged. Track Redis's process run ID
(`INFO server`) from successful mount registration and invalidate the mount
on a changed or unverifiable run ID; the heartbeat command adapter must
fail closed if that continuity check cannot be performed. A stable run ID
is **necessary but not sufficient** to prove Redis never rolled back or a
split brain did not occur, and reading it separately from a native renew
is not an atomic failover fence. Therefore a deployment that can transparently
switch/write to another primary, lose acknowledged state or restart between
continuity checks remains **unsupported** until a stronger lineage/fencing
protocol exists. Do not claim INFO + `PEXPIRE XX` is consensus. Prefer
detectable lock-subsystem degradation over silent lock resurrection, without
turning a Redis continuity failure into an automatic FUSE unmount.

**TDD dependency boundaries:** inject a small monotonic-clock/Redis-command
boundary at the *component construction or existing test dependency*
layer—do not add production-only testing setters or a generic Session API.
Exercise deterministic local cutoff, unknown responses, lock-only degradation,
reply-time clock/epoch checks and explicit normal unmount cleanup without
real-time 45/60-second sleeps. A separate **real Linux/FUSE E2E**
test must force loss of renewal while requests are queued and verify that
the FUSE mount stays accessible, lock requests return errors, ordinary
file operations continue when their backends are healthy, and the heartbeat
worker cannot outlive a subsequent **user-initiated** FUSE unmount.
Memory remains an in-process validity equivalent without a Redis heartbeat.
Capture lock-epoch loss reason and last confirmed lease deadline in diagnostics;
avoid logging tokens/nonces as credentials or claiming that owner TTL
replaces strict application/data-path fencing.

**#429-only implementation readiness boundary:** the first stage includes
format-time counter creation, registration, renewal, expiry, local
`LOCKS_UNAVAILABLE`, bounded normal-`umount` lease revocation, configurable
lease timing and Memory/Redis lifecycle tests. It does **not** implement
inode lock-table operations or enable the FUSE remote-lock flags. An epoch
that enters `LOCKS_UNAVAILABLE` remains unavailable until a **fresh mount**
in the first stage; automatic same-mount rearm is deferred until kernel
stale-lock/reacquisition semantics have a separately proved contract.
#430 later consumes the lease validity predicate in its atomic lock-grant
authority; its lock-table representation, GC, conversion, wait and operation
journal design are **not prerequisites** for #429 test-first implementation.

**Counter corruption safety:** a naive standalone `INCR` mutates a preexisting
negative counter before an application can reject its value. For example,
if that Redis key held `-2`, successive increments yield `-1`, `0`, `1`;
rejecting only each post-result value can eventually reuse generation `1`.
The watched pre-read protocol **must abort before queuing INCR** when
the old counter is negative, malformed or at `INT64_MAX`. This is worth one
mount-time optimistic transaction rather than inventing a volume-wide
quarantine marker to repair a locally observed corruption error. Unit tests
must prove the Redis key remains unchanged across repeated failed attempts.

**Integration and replay details:** per-mount generation allocation and
confirmed registration must be completed once for the owner-enabled FUSE
mount startup path; while #429/#430/#431/#432/#433 are unfinished, keep
`FUSE_CAP_POSIX_LOCKS` and `FUSE_CAP_FLOCK_LOCKS` disabled. The `INCR` counter
is a durability-critical value, so `FLUSHDB`, failed restoration of that
counter, or Redis failover to stale history invalidates the single-authority
assumption. A routine reconnect to the *same trusted primary* may reissue
`PEXPIRE XX` before local cutoff. A reconnect after suspected history
rollback/primary change **must invalidate the lock epoch**; a last-minute
`PEXPIRE` success alone does not prove the history lineage. No automatic
multi-primary failover or Redis Cluster guarantee is advertised. Remote
lock initialization must not add a heartbeat to mounts on which remote
locking is intentionally disabled.

**#429 TDD entry contract (designed, not yet verified):** deterministic
Memory/Redis tests for watched nonnegative `INCR` allocation and gaps,
negative-counter rejection *without mutation*, unconfirmed registration,
single-use `SET NX PX`, on-time/late `PEXPIRE XX`, expiry without sweeper,
revoke-vs-renew, delayed/unknown results, old/new generation isolation,
Redis outage longer than the **configured** local validity window, FUSE
admission/reply cutoff and
simulated clock drift/suspend. Test the 10/60/15-second defaults with an
injected clock rather than magic sleeps. Verify `CLOCK_BOOTTIME` and bounded
shutdown under the real Linux FUSE runner. #430 must separately prove
watched-key TTL expiry aborts stale grants, expired-holder filtering is
atomic with grant and cleanup cannot erase newer records. No FUSE remote-lock
capability is enabled by #429.

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

The **linearization point** is one Memory serialized transition or a
successfully executed Redis transaction (prefer native WATCH/EXEC once its
expiry predicate is proven; narrowly scoped Lua remains an alternative).
Lock operations atomically:

1. Validate that the requesting owner incarnation has a currently valid
   lease, and exclude expired/revoked competing owners, in the **same
   authoritative time/transaction domain** as the grant (never from a stale
   daemon-local cache or a separate preflight call).
2. Check relevant lock conflicts and class-specific owner replacement,
   insertion, splitting, or removal against the same authoritative snapshot.
3. Mutate the lock record and inode change sequence together, including any
   operation identity/outcome for replay and optional same-inode lazy
   reclamation of invalid generations. No owner-wide index is necessary.

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
LOCK_UNREGISTERED --register--> LOCK_ACTIVE --confirmed renew--> LOCK_ACTIVE
LOCK_ACTIVE --local cutoff / server invalidity--> LOCKS_UNAVAILABLE (FUSE stays mounted)
LOCK_ACTIVE --explicit umount--> QUIESCE_LOCKS --> stop heartbeat / DEL lease --> FUSE unmount
LOCKS_UNAVAILABLE --best-effort old-generation revoke/GC--> stale records removed
```

Invalidity is effective *on expiry at the authority*, not when a cleanup
worker eventually deletes keys. Every renew and lock mutation checks the
same identity and expiry predicate. Stale operations cannot resurrect the
owner or remove newer incarnation records. Physical GC is optional and
separate; an unbounded stale-record accumulation is still unacceptable.
The **lock subsystem** must reject use of the expired owner epoch by its
conservative local cutoff; the FUSE mount stays active and any future new
lock epoch requires explicit reacquisition, not restoration of old locks.
Redis lease expiry alone cannot fence a partitioned
application's pre-existing critical section; the explicit scope/limit is
defined above.

### Waiting, wakeup, cancellation and reply ownership

Nonblocking operations perform one conditional grant. Blocking `F_SETLKW`
and blocking `flock` use a bounded **wait/recheck loop**, not a long-lived
Redis transaction or polling spin. A waiter observes a per-inode version,
subscribes to a wakeup channel, then rechecks version/authority **after**
subscription and before sleeping. Redis notifications only improve latency:
missed notifications must recover through version comparison and a bounded
recheck interval. Only a new atomic conflict check can grant the lock. Bound
queued waiters, memory, and notification fanout; strict FIFO is not promised.

Enable interrupt handling by changing `conn->no_interrupt = 1` when locking
support is activated. `fuse_req_interrupt_func` registers without losing
already-arrived interrupts. Its callback only updates thread-safe
cancellation state and wakes the fiber-owned waiter; it must not acquire a
fiber mutex or use request memory after unregister/destruction. The request
fiber coordinates exactly one reply and interrupt-handler removal:

```
NEW -> CHECKING -> WAITING --wake/recheck--> CHECKING
         |           |
         +-----------+--interrupt--> CANCELLING -> DONE_INTERRUPTED
CHECKING --atomic grant--> GRANTED_PENDING_REPLY
GRANTED_PENDING_REPLY --successfully delivered--> DONE_GRANTED
GRANTED_PENDING_REPLY --cancel/failed delivery--> RESOLVE_UNDELIVERED
RESOLVE_UNDELIVERED --safe compensation--> DONE_WITHOUT_NEW_GRANT
```

An interrupted waiter must not leave a **ghost lock**. If the backend grant
committed but no success reply was delivered, compensate with the **exact
operation/grant identity**, never by blindly removing all locks for that
owner after a newer operation could have succeeded. Do not reply `EINTR`
while still leaving an in-flight request eligible to acquire a lock.
**A lock conversion complicates compensation**: undoing a newly acquired
range can differ from restoring a prior same-owner range, and another owner
may have acquired an intervening compatible lock. Conditional compensation
must validate the owner revision and the whole affected conflict state; a
reply failure cannot justify overwriting newer lock grants to restore the
old shape. If a conversion cannot be safely reversed, the protocol needs an
explicitly justified failure/owner-revocation policy before activation.
The Redis commit and kernel reply are not one atomic transaction; an
unqualified claim of perfect cancel-and-restore semantics would be false.
Interrupted/cancelled, success-delivered, ambiguous reply-delivery, and
compensation-error cases require explicit deterministic tests. A late
interrupt after a successfully delivered reply cannot retroactively
withdraw the successful grant. Deadlock detection/`EDEADLK` is **not**
automatically provided by this wait loop; investigate pinned workloads
before claiming Linux-equivalent cross-mount deadlock detection.

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
would not remove it at the right time. #379/#431 must settle a safe strategy
(including whether the first-stage POSIX-only target is possible with this
transport) **before** enabling `FUSE_CAP_POSIX_LOCKS`. Merely declining to
advertise OFD support cannot filter these requests.

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

For #429 the first-stage *design* uses native Redis 7.x WATCH-validated
`INCR` / `SET NX PX` / `PEXPIRE XX` / `DEL`, with configurable defaults of
60-second owner TTL, 10-second renew cadence, 15-second local safety
reserve and 5-second attempt deadline. Lock-only degradation,
reply-time validation and **O(1)-key lease revoke at explicit `umount`**
are specified above; per-inode stale records are reclaimed only on later
lock accesses, not by unmount enumeration. Validate against Redis 7.x and real FUSE
lock degradation/ordinary unmount via TDD before treating the protocol
as implemented. For #430, prove `WATCH` expiry-at-`EXEC` plus full watched
lease set under load, then finalize operation-ID retention, lazy-expired-lock
filtering and bounded physical GC (or justify narrowly scoped Lua if native
optimistic transactions fail their proof). Document the bounded
waiter resource policy, **conversion
compensation**, and exact failure responses before the implementation is
deemed ready. Protect the following
with Memory/Redis tests and end-to-end / two-mount tests: lock range
split/merge, read/read and read/write, owner upgrades, `GETLK` PID
limitations, POSIX any-close, flock last-close, `LOCK_NB`, wait and wake,
missed wakeup, cancel-before/after grant, ambiguous commit, stale owner
cleanup, Redis outage/restart, and reply-delivery failure. Tests must await
semantic completion, not use sleep as proof of correctness.

Promote `generic/131` and `generic/504` only on authoritative CI evidence;
evaluate `generic/478` separately as mixed POSIX/OFD coverage. Delegation
cases remain unsupported. Do not enable `FUSE_CAP_POSIX_LOCKS` or
`FUSE_CAP_FLOCK_LOCKS` merely because the corresponding callbacks exist.

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
Unlink. Memory/Redis compare the current directory entry to this identity
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
preparation therefore runs in resumable bounded batches. Memory metadata keeps
a snapshot continuation and Redis keeps an HSCAN page/cursor continuation
across worker batch boundaries. The worker can abort an in-flight candidate
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
rejects removals by default. An intentional contract change must be made
explicit with the `conformance-semantic-change` PR label so the exceptional
removal is visible in review. A known gap that starts passing is XPASS and must
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
