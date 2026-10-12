# #430 — Redis advisory-lock authority: implemented design

**State:** Redis-only C++ advisory-lock authority **implemented** in Draft
PR #461. Atomic grant/release, watched lease validation, bounded ambiguous-
EXEC retry, read-only GETLK, and lazy GC are tested; FUSE lock capability
activation **remains unimplemented** and belongs to #431–#433. A subsequent
independent review identified a mixed-live/stale HSCAN pagination bug; the
cursor-continuation correction was verified by TDD RED/GREEN and full PR CI
in #38258486564. Parent
[#379](https://github.com/SwordInfra/SwordFS/issues/379);
prerequisite [#429](https://github.com/SwordInfra/SwordFS/issues/429)
merged in [#460](https://github.com/SwordInfra/SwordFS/pull/460).
The shared POSIX/flock contract remains in
[`posix-conformance.md`](posix-conformance.md). This document owns only
#430's Redis grant, persistence, idempotency and lazy-reclamation design.

## 1. Scope and architectural decisions

1. Redis **only**. Do not restore the deleted Memory backend, add a second
   metadata engine or create a generic Session abstraction. #429 has already
   implemented never-reused `(volume, generation)` identities, the TTL lease,
   `CanGrant`, and O(1)-key normal-unmount `DEL`.
2. **Authority-side** G1–G4: one Redis 7 single-primary history serializes
   mutually exclusive grants; lease validity is checked at authorization;
   older generations cannot mutate newer records; revoked/expired grants are
   ignored before physical GC. This does **not** fence an application that
   continues its critical section after a previously acknowledged lock.
3. Implement a single narrow Redis lock-authority API that can later be
   called from #431 POSIX and #432 BSD-flock VFS hooks. #430 alone does not
   enable `FUSE_CAP_POSIX_LOCKS` or `FUSE_CAP_FLOCK_LOCKS`, implement waits,
   or claim OFD/exceptional-close support (#431–#433).
4. **First choice: native `WATCH` / `MULTI` / `EXEC`.** The transaction must
   protect every key whose *observed* state determines the outcome; native
   Redis expiry of a watched key aborts EXEC. Use a scoped Redis grant script
   only after correctness or realistic contention/performance evidence shows
   the native protocol is inadequate. No Lua is needed for #429 renewal.
5. **Normal full unlock deletes its owner field immediately.** Do not
   preserve one Redis tombstone per historically touched inode merely for
   request deduplication. Full release uses `HDEL`, partial unlock updates
   the remaining ranges via `HSET`, and a naturally empty Hash vanishes.
   A lost Redis `EXEC` reply is not, by itself, a reason to terminalize the
   whole lock generation: prefer bounded same-request retry after proving
   state-setting idempotence and old/new WATCH ordering as below.
6. No Owner→inode global reverse index, per-lock TTL, mandatory full-volume
   sweep, or per-lock Redis work on ordinary file reads/writes. Normal
   `umount` remains only the already-implemented owner-lease revocation.

**Reference evidence.** Redis documents that watched-key expiry (since
6.0.9) and renewal/revocation abort `EXEC`, and that Redis transactions do
not roll back individual command-level runtime failures:
[Redis transactions](https://redis.io/docs/latest/develop/using-commands/transactions/),
[watched expiry](https://redis.io/faq/doc/11ed4zroc6/can-a-key-expire-within-a-multi-exec-transaction).
JuiceFS's [Redis locking implementation](https://github.com/juicedata/juicefs/blob/main/pkg/meta/redis.go)
uses per-inode POSIX/flock Hashes, `(sid, owner)` identity and an owner-wide
reverse index. Its reverse index and session-wide cleanup are **not** adopted:
SwordFS treats a generation's TTL lease as the single liveness authority.

## 2. Candidate schema and key ownership

All example suffixes below are inside the **existing**
`RedisKey` namespace, e.g. `{0:volume}:...`, not a new Redis database.
Persistent `owner_generation` and `owner_lease:<generation>` are #429's
**already implemented** keys; their layout and TTL are not redefined here.
**Only the Owner Lease carries a TTL. Neither POSIX nor flock Hashes have
a TTL.** Authority-side `PTTL` always targets
`owner_lease:<generation>`, never an individual lock. Redis decides whether
the lease has expired using its own expiration clock; there is no
cross-Mount comparison of client clocks. `EXISTS <lease>` could check
presence, but `PTTL > 0` additionally rejects a malformed **nonexpiring**
lease (`PTTL = -1`), while `-2` means already absent/expired. The holder
lease is WATCHed before its PTTL; Redis 7 aborts EXEC if this watched
lease expires, is renewed or revoked between the read and the commit.
#429's per-client `CLOCK_BOOTTIME` is a separate conservative **local
admission/reply** cutoff, not the clock used to judge another Mount.

| Suffix | Redis type | Value / invariant | TTL |
| --- | --- | --- | --- |
| `lock:posix:<ino>` | Hash | One field per currently locking `(generation, kernel_owner)`, containing normalized POSIX intervals | None |
| `lock:flock:<ino>` | Hash | One field per currently locking `(generation, kernel_owner)`, containing SH/EX whole-file state | None |

An **owner field** is a canonical, bounded encoded lock state, not a
historical operation receipt:

```
owner_field = encode(generation, opaque_kernel_lock_owner)
owner_value = canonical_lock_state // POSIX: disjoint sorted ranges; flock: SH/EX
```

No `@version` field is reserved in #430: Redis WATCH already serializes
mutations of the inode Hash. #433 may design a separate **non-authoritative**
waiter change notification/version, without forcing one permanent lock
record onto every historically touched inode. When the last field is
removed, Redis removes the empty Hash key automatically.
Use a strict, versioned and bounded encoding with explicit class,
interval bounds and size limits. The implemented POSIX codec is a canonical
`P1|R,start,end|W,start,end` ASCII interval set; the flock codec is `F1|S`
or `F1|X`. A missing record means no lock; corrupt or oversized values
fail closed instead of being reinterpreted as an unlocked inode. `l_len=0` has an explicit infinity
sentinel; conversions and partial unlocks normalize/merge/split ranges.

**Why no mandatory tombstone or operation journal?** POSIX range
replacement/unlock and BSD flock SH/EX/UN are *state-setting* operations:
the canonical owner-state transform `f(request, state)` must satisfy
`f(request, f(request, state)) = f(request, state)` for an otherwise
unchanged same-Owner state. A repeated request is **not** an append, a
decrement, or a second independently acquired lock. #430 serializes all
modifying operations within `(generation, kernel_owner, inode, class)`;
it does not permit a **different** operation to overtake an unresolved
earlier one. A normal successful complete unlock therefore safely removes
the Owner field; no historic tombstone is required for a fresh later
application lock request.

**Unknown EXEC result: same-request retry is the preferred candidate,
subject to the WATCH ordering proof.** Keep the identical generation,
opaque owner, class, inode, normalized request and mode; create a **new
WATCH/read/conflict-check/MULTI/EXEC attempt**, not an unguarded repeat of
the previously calculated HSET value. Before retrying, recheck both the
local #429 admission gate and authoritative requester Lease. Keep later
**different** same-Owner operations queued until this request has reached
a definitive safe outcome. No per-Owner global serialization is necessary
for operations on unrelated inodes/classes.

Consider T1's uncertain `EXEC` and retry T2 on the **same inode Hash**:

- If T1 committed first, T2 sees the resulting owner state; the identical
  state-setting request is idempotent, while other holders are rechecked.
- If T2 modifies the Hash first, T1's earlier `WATCH` of that Hash causes
  the subsequently arriving T1 `EXEC` to abort. The same rule applies
  when a later different T3 operation modifies that Hash.
- An unchanged **HSET** still modifies the watched Hash. A successful
  **absent-field HDEL does not**: the later T1 might grant an already
  released lock after the HDEL reply. A later T3 mutation cannot protect
  the interval between that no-op unlock and T3.
- Every attempt also WATCHes the requester Lease; after expiry/DEL an old
  attempt cannot newly authorize a grant. Different Mount generations
  cannot reuse each other's owner fields.

**Verified Redis 7 counterexample and narrow solution (2026-10-12):**
`scripts/testing/redis-lock-authority-probe.py` deliberately leaves an
owner HSET in a WATCHed transaction pending, commits a subsequent `HDEL`
of that still-absent owner field (reply `0`), then commits the original
HSET: a ghost lock appears. For a no-op whole-owner unlock, queue an
**ephemeral same-Hash write fence** in the same MULTI/EXEC: HSET a reserved
temporary field, HDEL the requested owner field, then HDEL the temporary
field. The probe verifies that this invalidates the older WATCH even when
the Hash is absent both before and after the transaction; no marker,
tombstone or journal survives. This is a narrowly justified exception to
the otherwise single-mutation authority transaction. The temporary field
must be outside the valid owner-field codec; it is never treated as an
active holder. Reject any preexisting reserved marker during validation;
all three queued commands and their EXEC replies require explicit error
handling (Redis transactions do not roll back successful subcommands).
For nonempty owner-field full unlock, a single HDEL remains sufficient.

This demonstrates a concrete native-WATCH solution **without** an
indefinite outcome journal, but full C++ TDD still must verify all failure
classifications, lock-interval state transitions and exact late-EXEC
interleavings before claiming G1–G4. Do not terminalize a generation merely
because an EXEC reply is lost.

**Decision — bounded retries exhausted:** return an explicit lock-operation
error (typically `EIO` for uncertain Redis transport), **without**
terminalizing the Owner generation, revoking the lease, automatically
unmounting FUSE, or creating outcome journals/tombstones. Keep the ordinary
Mount and #429 heartbeat unchanged. The returned error means **the result
was not confirmed**; it does *not* guarantee Redis made no change. A
possibly committed grant can remain authoritative while this Mount's
lease is valid, until a later normal unlock/close or lease revocation/
expiry. Never report successful acquisition or definitive non-application
based solely on retry exhaustion.

This intentionally does **not** promise exactly-once results or automatic
cleanup of unacknowledged grants. It also does not by itself prove a
new **different** same-Owner request is safe after an unresolved older
EXEC: in particular an absent-field `HDEL` can be a no-op, so an earlier
watched HSET may not be invalidated. Do not silently claim that local
serialization lasting only until the error reply resolves this case.
Keep this late-commit/no-op interaction as a specific #430 correctness
review/test boundary, without introducing new recovery machinery now.

**Storage alternatives, not silently adopted:** an inode-local generation
Set plus per-`(inode,class,generation)` Hashes can reclaim many owners from
one dead generation via `UNLINK`, but costs extra keys/reads/transaction
mutations, and its Set still grows with cold generations. A flat per-inode
Hash is the proposed **simple starting representation**. This choice is
conditional on #430's required high-stale-density performance evidence in
section 7; do not describe it as proven scalable before that gate passes.
With normal unlock actually deleting its fields, only **expired/umounted
generation** records remain as lock-table residue. Neither layout
guarantees a global physical storage bound for never-revisited cold inodes.

## 3. Grant/modify transaction and Redis command ordering

The following is a **logical protocol sketch**, not exact redis++ syntax.
All commands are issued on one checked-out Redis connection. The current
`RedisKvTxn` API lacks `PTTL` and whole-Hash snapshots; #430 may extend
it narrowly rather than exposing Redis internals to VFS.

1. Validate and normalize the request before Redis admission: supported
   class/mode/range, stable generation and opaque kernel owner; check
   #429's local `CanGrant(generation)`.
   A missing/expired/terminal requester cannot succeed.
2. `WATCH <inode_class_hash> <requester_lease>` **before** reading either.
   Read the owner fields (`HGETALL` for the baseline representation);
   reject malformed/inconsistent type or codec.
   Read the **requester's Owner Lease** `PTTL`, requiring positive TTL
   (not `-1` or `-2`). The inode lock Hash is never assigned a TTL:
   a missing inode Hash represents **no current locks**, not a missing
   generation lease.
3. Discover potentially conflicting holders from the *watched* inode Hash.
   For each relevant generation, `WATCH <holder_lease>` **before** its
   `PTTL`. `PTTL > 0` means the holder's lock participates in conflict
   checks; `PTTL <= 0` makes it inert. Every `PTTL` reads the **holder's
   Mount lease key**, not an individual lock. Same-owner locks are transformed,
   not treated as competitors. Group checks by generation, not field, to
   avoid repeated lease RTTs for many owners in one mount.
4. Check the POSIX read/write interval intersection or flock SH/EX rule.
   Compatible shared grants may coexist. Same-owner conversions atomically
   replace the *canonical owner state*; `UNLCK` removes/splits only that
   owner's ranges. The result is either a nonempty canonical owner state,
   or **no remaining lock at all for that Owner**.
5. Queue `MULTI`, then normally exactly **one Redis lock-state mutation**:
   `HSET <hash> <owner_field> <canonical_state>` for a grant, conversion
   or partial unlock; `HDEL <hash> <owner_field>` for a full unlock.
   `EXEC` atomically commits the single mutation if every WATCH condition
   still holds. `HDEL` of the last owner field removes the Hash automatically.
   When the owner field is **already absent** (a semantically no-op full
   unlock), use the three-command ephemeral same-Hash write fence described
   in section 2 to invalidate older pending grants. No separate permanent
   journal or version write is in this atomicity domain. Prevalidate Hash
   type/codec; inspect **every** EXEC subcommand reply. Any command-level
   EXEC error is an unconfirmed authority outcome, never a presumed rollback
   or success. The temporary field must not be left behind on a confirmed
   successful transaction.
6. A WATCH abort means **retry the entire snapshot and conflict calculation**
   within a bounded retry/backoff budget, never grant on stale data.
   An opaque/lost reply after a **mutating EXEC** is also eligible for
   bounded **same-request**, full-snapshot retry under section 2's
   ordering and idempotence proof; it is not permission to submit a
   later different same-Owner request or blindly resend old encoded state.
   A semantic lock conflict is **not** a transaction retry: return
   `EAGAIN`/`EACCES` or `EWOULDBLOCK` for nonblocking requests. A conflict
   path may validate the watched snapshot through read-only EXEC, with no
   owner-state write. If its response is lost, the caller may issue a fresh
   semantic attempt (which may now succeed) but cannot claim that an
   earlier grant was committed.
7. On a successful commit, the #431/#432 caller must recheck the **original**
   generation and #429's local cutoff immediately before a successful FUSE
   reply. Failure of this check means no success is reported; a committed
   modification is **not blindly rolled back**. Treat cancellation and
   reply-transport failure according to the explicit boundaries below.
   This cannot fence an already-running application.

`GETLK` is a **read-only**, watched snapshot using the same owner-validity
checks for a reported blocker. It must validate the watched snapshot at
`EXEC` even when there are no writes; reading Hash/PTTL and returning a
result without a final WATCH validation is unsafe. An absent blocker is
returned as `F_UNLCK` only after all potentially conflicting **live**
holders have been checked. If the requester's lease is invalid, fail
closed, including for `GETLK`.

**No automatic GC in the grant critical section is required for safety.**
Additional stale-field `HDEL` commands inside the grant transaction would
reintroduce partial multi-command error handling; run independently
conditional GC against already-inert generation fields instead. GC is an
optimization and may safely fail without reviving conflicts.

## 4. Outcomes, conversions and reply-delivery boundaries

| Authority outcome | Required behavior |
| --- | --- |
| Confirmed EXEC + valid local generation | Return committed semantic result; reply delivery is handled by #431–#433. |
| WATCH conflict | Re-evaluate all keys and TTLs; bounded retry, no speculative grant. |
| Confirmed semantic conflict | No grant; report nonblocking conflict. |
| `OutcomeUnknown` for a **mutating** EXEC / lost Redis acknowledgement | Bounded same-request re-WATCH/read/PTTL/conflict-check/EXEC retry. On exhaustion return a lock-operation error; **no** generation termination, lease revocation, FUSE unmount or durable result journal. The Redis mutation may have committed despite the error. Late original EXEC versus a later different same-Owner operation remains an explicit proof boundary, especially if the latter's HDEL is a no-op. |
| Owner lease expiry/revocation while recovering | No new successful grant or replay; old record becomes inert, regardless of its last stored result. |
| Next **different** same-Owner request while prior attempt/retries are active | Serialize while the attempt/retries are executing. If retries exhaust, return an error without pretending the old server-side attempt was definitely absent; late-EXEC interaction with subsequent operations must be tested and is not guaranteed by serialization alone. |

**FUSE reply-transport failure is different from Redis outcome ambiguity.**
`setlk`/`flock` respond with `fuse_reply_err(req, 0)` after a successful
authority mutation, and libfuse returns `-errno` when it **cannot send**
that response. The libfuse low-level API notes that a reply-sending error
normally signals an unrecoverable kernel FUSE connection; it is not a
normal Redis failure or proof that an ordinary healthy process failed to
receive a lock. A forced unmount, closed FUSE device/connection or request
cancellation/interruption can create an unavailable or non-deliverable
request. `fuse_reply_err` returning 0 confirms submission to libfuse/kernel,
**not** that application code later ran or observed the response.

**Minimal policy:** do not automatically reverse a Redis lock conversion
because sending its FUSE reply failed. For example, a committed POSIX
EX→SH downgrade may already have allowed a second SH owner; converting
the first owner back to EX would create conflicting grants. Preserve the
already committed lock state until normal delivered close/unlock, or
invalidate the entire Mount generation when its FUSE connection actually
terminates, using #429's exact lease revoke/TTL fallback. This avoids
inventing arbitrary per-lock rollback and protects other Owner grants.
The finer **interrupt-versus-grant** state machine when FUSE is still
healthy (especially blocking requests) belongs to #433; do not mistake
an interrupt for a confirmed failed reply or make #430 depend on finished
blocking-wait design.

## 5. Expiry, no-scan umount and lazy reclamation

- `DEL owner_lease:<generation>` on normal `umount` invalidates **all** its
  POSIX/flock records immediately in the authority's predicate, in O(1)
  **lease keys**. #430 adds **no reverse index, enumerator or per-lock
  shutdown loop**. A lost DEL reply relies on bounded Redis TTL expiry,
  not a claim of synchronous unlock.
- On the next inode lock access, read the recorded generation, WATCH its
  lease before `PTTL`, then exclude absent/expired generations regardless
  of lingering Hash fields. The normal grant may proceed without physical
  HDEL. That access can also initiate a **bounded, best-effort cleanup
  pass** for expired generations on the *same inode*, after the grant/read
  is validated; it need not hold up a valid grant while sweeping. The
  cleanup attempt must WATCH/re-read the inode state before exact-field
  `HDEL` and not erase new data. A lease key cannot be revived or its
  generation reused.
- Successful full unlock removes its owner field via `HDEL`; if the Hash is
  empty, Redis deletes the key. This is not a special GC pass and does not
  wait for owner-lease expiry. No active-generation tombstones remain.
- Cold inode Hashes may remain indefinitely if never touched. This model
  guarantees **logical** expiry, **not bounded total Redis memory use**.
  Physical retention bounds, if required, need separate evidence for a
  low-amplification maintenance policy, not a surprise unmount sweep.

## 6. Execution-domain/API boundary

Reuse the existing `RedisBackendContext` blocking executor and
`RedisMetaClient`/`RedisKvTxn` transaction machinery. Actual Redis I/O runs
in the **thread** execution domain, never directly on a FUSE fiber.
`metadata/LockAuthority.hpp` defines the narrow **backend-neutral semantic
interface** for #431/#432 to consume without coupling VFS to the Redis Hash
codec. `RedisLockAuthority` is its sole implementation; this is not a
generic Redis session, alternative metadata engine or extensible backend
framework. Virtual dispatch here is outside the I/O hot path and the only
polymorphic boundary to the later POSIX/flock hooks.
Expose only `SetPosix` (including unlock), `GetPosix`, and `SetFlock`
(including unlock); do not make
FUSE or the generic `IMetaEngine` parse per-inode Redis fields. Share the
owner identity and generation validity contract with #429, not an
independent session lifecycle. A retrying lock operation must not block
ordinary read/write/truncate or acquire a mount-global lock.

## 7. Historical pre-implementation design-review gates (completed evidence)

**P0 correctness proofs** (deterministic Redis 7 tests in later TDD):

1. Watch requester/inode, discover *all relevant* holders, watch each
   holder lease before PTTL, and prove renew/DEL/expiry between PTTL and
   EXEC cannot produce two conflicting live grants. Include simultaneous
   shared grants and incompatible range conversions.
2. Prove `HSET` and normal complete `HDEL` operations under Redis faults,
   including an **unknown EXEC followed by the same-request retry**,
   delayed original EXEC before/after retry and later different
   same-Owner request, no-op unlock of an absent field, wrongtype and
   malformed lock values. Prove old watched state cannot overwrite a
   later update and retry never invents a success under an expired lease.
   When retry budget exhausts, assert **error only**, with no implicit
   lease revoke/terminal transition and no false "lock absent" claim;
   test that a possibly committed grant may remain until normal release
   or owner-lease invalidation.
3. Verify that a failed FUSE reply after a committed POSIX conversion does
   **not** attempt a blind reversal. Integration must prove close/unmount
   cleanup and not conflate a kernel interrupt with transport failure.
4. Prove `GETLK` read-only EXEC verification and missing/expired requester
   rejection. Do not equate locally observed PTTL with commit validity.

**P0 operational/performance evidence** (a small focused Redis workload,
**not** a production benchmark claim made by this review): compare flat
Hash against inode-local generation grouping at e.g. 1, 32, 1,000 and
100,000 stale owners, including hot-inode contention, read/read fanout,
range-heavy writers, and a cold-inode revisit after bulk mount expiry.
Measure Redis round trips, HGETALL bytes, EXEC abort/retry rate, p99
latency and retained memory. Include a **long-lived healthy generation**
that locks/unlocks many distinct inodes while holding no locks: it should
not accumulate owner lock records or per-inode tombstones. Define limits for per-inode owner
field count, encoded state size, bounded lazy cleanup batches and
**fail-closed** overflow (`ENOLCK` or another agreed resource error);
never skip unseen fields and falsely grant a lock. If the baseline cannot
meet the target, revise the schema or prove a scoped Lua alternative
**before implementation**, not after enabling the capability.

**Dependency boundary:** #431 owns delivered `FLUSH`/traditional POSIX
close semantics and untagged OFD FUSE transport; #432 owns flock last
`RELEASE`; #433 owns blocking wait/interrupt and capability activation.
Their details must not prevent reviewing this Redis authorization core.

## 8. Design-review disposition

**Historical design decision (before C++ implementation):** the Redis 7
experiments in section 9 supported moving to TDD on PR #461. The resulting
production authority is implemented and tested; current CI and remaining
review gates are tracked in the implementation checkpoint below. The
architecture uses (a) **normal HDEL and no retained
tombstones**, (b) no automatic conversion rollback on FUSE reply failure,
and (c) **bounded same-request retry followed by an error on exhaustion**,
without generation-wide revocation or unbounded deduplication metadata.
The verified missing-field HDEL counterexample requires a transient write
fence (section 2); high-stale-density measurements favor the flat Hash with
strict admission caps and bounded lazy GC. These decisions led to the
implemented C++ RED/GREEN behavior. The later independent review's HSCAN
pagination finding was resolved by bounded cursor continuation, verified
in focused Dev Build #38258479625 and full CI #38258486564.

## 9. Redis 7.4.11 empirical protocol / scaling checkpoint (2026-10-12)

Run `python3 scripts/testing/redis-lock-authority-probe.py --benchmark`
against **only a disposable, exclusive Redis 7.4 instance**, since the
probe uses destructive FLUSHDB. For example, map an isolated Redis container
port to localhost 16379. The probe is a script-based architecture experiment,
**not** the required production C++ RED/GREEN tests.

**Correctness:** confirmed with deterministic WATCH/MULTI/EXEC command
interleavings: identical HSET retry invalidates delayed EXEC; absent-field
HDEL does **not**, permitting a ghost grant; the ephemeral three-command
same-Hash fence prevents it with zero surviving lock fields; requester lease
renewal, DEL and expiry invalidate watched grants; holder renewal invalidates
a candidate grant; GETLK's read-only watched EXEC rejects a changed snapshot;
simultaneous exclusive contenders serialize; compatible SH owners succeed
after WATCH retry; watched GC cannot erase a changed owner field; 128 normal
lock/unlock inode lifecycles retain no lock keys. A Hash of 1,125 fields
(1,124 stale and one live) triggers fail-closed size detection; bounded
HSCAN/HDEL cleanup reduces it below the proposed 1,024-field cap without
deleting the live owner.

**Single-process, no-load discovery cost**, measured ten times per point.
Reported maximum is the **largest of ten samples**, not a statistically
credible production p99. Memory includes Redis process baseline:

| Old owner fields | Flat Hash (max ms / keys / bytes) | Generation-grouped Hashes (max ms / keys / bytes) |
| ---: | ---: | ---: |
| 1 | 0.23 / 1 / 1,357,648 | 0.36 / 2 / 1,407,096 |
| 32 | 0.43 / 1 / 1,407,760 | 0.60 / 33 / 1,410,120 |
| 1,000 | 2.24 / 1 / 1,471,240 | 9.97 / 1,001 / 1,517,280 |
| 100,000 | 286.62 / 1 / 8,581,776 | 1,118.51 / 100,001 / 13,342,784 |

Flat HGETALL transferred ~2.09 MB at 100,000 fields versus ~1.79 MB for
grouped discovery, whose many small Hashes nevertheless caused far higher
latency and key-count amplification. Neither permits unlimited stale-holder
enumeration in a lock request. Keep the **flat Hash** as the beta baseline,
with the following **proposed fail-closed bounds** for the C++ contract:

An additional 100-transaction hot-inode exercise with 128-range values,
synthetic competing writers and holder-lease renewals produced a 30%
WATCH-abort rate for each field-count sample (by construction):

| Owner fields | Grant-attempt sampled p99 | 4-concurrent-reader GETLK sampled p99 |
| ---: | ---: | ---: |
| 1 | 1.76 ms | 2.15 ms |
| 32 | 1.78 ms | 3.11 ms |
| 1,000 | 8.67 ms | 32.39 ms |

These are local, synthetic Redis command-level probes; they exclude C++
codec/dispatch costs and network latency. They support a finite size budget,
but do not establish acceptable end-to-end production p99 or Redis failover
guarantees.

**Lease-validation scaling refinement (2026-10-12).** A flat Hash of 1,024
different active generations is itself a bounded data structure, but one
`WATCH`/`PTTL` round trip per distinct generation measured **284.80ms median**
even on localhost. `RedisKvTxn::PttlBatch` instead WATCHes all distinct
generation keys in one call and uses one **read-only Lua evaluation** to
collect PTTLs before the watched EXEC. Every holder is still validated;
none is authorized from a truncated sample or an un-WATCHed cached TTL.

| Distinct holders | Sequential WATCH/PTTL median | Batched WATCH/EVAL median |
| ---: | ---: | ---: |
| 32 | 9.83ms | 1.29ms |
| 128 | 35.92ms | 2.01ms |
| 512 | 143.31ms | 5.73ms |
| 1,024 | 284.80ms | 14.22ms |

The native probe checks that a renewal following the read-only batch
invalidates EXEC. `PTTL=-2` means expired/missing and eligible for bounded
stale GC; `PTTL=-1` is an unexpected permanent lease and fails closed;
`PTTL=0` is treated as still potentially live until WATCH confirms expiry
(to avoid deleting a lease with a sub-millisecond lifetime). The C++
implementation and test suite must retain these distinctions. This requires
Redis Lua/EVAL permission; if EVAL is unavailable, lock operations fail
closed rather than bypassing generation validation. Run the benchmark with
`python3 scripts/testing/redis-lock-authority-probe.py --lease-batch`.

- At most **1,024 owner fields per (inode, class)** for ordinary complete
  HGETALL validation; check HLEN under WATCH first. Reject overlimit
  authorization with ENOLCK, **never** inspect a truncated subset and grant.
  **Admission must enforce the post-grant cap:** when HLEN is exactly 1,024,
  replacing an already-present owner field is permitted, but a new owner's
  HSET must fail ENOLCK, even for otherwise-compatible shared locks. This
  check uses the already-watched Hash cardinality without an additional Redis
  round trip; expired entries count until bounded cleanup reclaims a slot.
- At most **4 KiB per owner encoded state**, including at most 128 canonical
  POSIX ranges. Corruption, unknown owner fields, or larger decoded values
  fail closed, not as an empty lock table. Numeric limits are engineering
  policy to test under CI rather than a Redis schema compatibility contract.
- When over the field-count cap or after observing expired holders, run
  **one watched HSCAN page per best-effort cleanup attempt** (COUNT=128 is a
  Redis hint, not a hard maximum). Inspect **every** field returned on that
  page so no field is silently skipped. WATCH the Hash and relevant Owner
  Lease keys, PTTL-check owners, then conditionally HDEL expired-generation
  fields. Only a successfully validated transaction advances the cursor;
  failures retry the same page on later accesses. A returned empty page with
  a nonzero cursor is still progress.
- Remember the next HSCAN cursor per (inode, lock class) in a **bounded local
  256-entry cache**. Repeated accesses to a retained cursor continue from
  the last page, including when the first page has only live holders. Scan
  cleanup is striped by Hash so same-Hash requests cannot regress one
  another's progress; a cursor reaching zero completes a cycle and resets.
  Eviction or restart only restarts a safe scan from zero; high churn across
  more than 256 simultaneously overflowing Hashes can delay progress.
  Hash mutations while scanning may require another pass. Redis cursors are
  an opportunistic reclamation aid, never an authorization shortcut: every
  incomplete/overlimit Hash remains **fail-closed** until below the limit.
  Cold inodes are not globally swept; no umount lock enumeration is needed.

**Implementation evidence:** numeric caps, WATCH lease validation, bounded
retry, and the mixed live/stale continuation regression are verified by C++
Redis tests. The independent review on PR #461 specifically found the
first-page-only GC bug and required this cursor-continuation fix. The temporary
fence commands are a single atomic Redis transaction: after a successful
first HSET on a hash key, the two HDELs on that same key are guaranteed to
be type-correct; EXEC replies still must all be inspected and an unknown
result must be reported as such. No new FUSE capabilities should be enabled
in this PR.


## Implementation checkpoint (Draft PR #461, 2026-10-12)

The Redis-only authority now lives in `RedisLockAuthority`, exposed to the
future #431/#432 VFS integration through `ILockAuthority`. The call boundary
is **thread-domain only**; FUSE lock capability flags remain off in this PR.
No memory-backend path or compatibility migration was introduced.

`SetPosix`, `GetPosix`, and `SetFlock` share the same watched Hash/owner-lease
snapshot rules. `RedisKvTxn` provides watched `HGETALL` and batched PTTL:
one WATCH over all distinct generation lease keys, followed by a read-only
Lua EVAL, then a checked EXEC. Read-only `GETLK` also validates EXEC before
publishing its conflict output. The producer serializes operations via
same-owner lock stripes and retries ambiguous commits with a finite budget.
The no-op unlock case queues a transient HSET/HDEL/HDEL fence in one EXEC
and must leave no retained marker. Expired generations may be reclaimed by
bounded watched HSCAN/HDEL; over-budget admission fails closed with ENOLCK.

Redis protocol experimentation and C++ tests are deliberately distinct.
The Redis 7.4.11 probe establishes WATCH/interleaving behavior, including
lost TCP acknowledgements, with command-level experiments. The C++ authority
tests exercise the actual Redis client against live Redis, including competing
owners, read-only GETLK, split/merge and flock conversions, generation
expiry/revocation, no-op HDEL fencing against delayed EXEC, bounded stale
cleanup, and the 1,024-owner fail-closed admission boundary. The first 19
focused tests passed in Dev Build #38230202301. Full CI #38230206097
passed with 92.13% aggregate patch line coverage and more than 90% for every
changed executable production C++ file.

The additional **test-only** RESP2 fault proxy forwards production commands
to the real Redis server and can lose a selected EXEC reply either **after**
the server commits or **before** the server sees EXEC. It exercises actual
C++ same-request replay of POSIX grants, flock retries exhausted after three
ambiguous replies with error-only outcome (without revoking Owner Lease),
and explicit subsequent flock conversion/unlock. Dev Build #38250295172
passed both applied/not-applied C++ fault tests. No test-only production hook,
client-side Redis mock, unconditional rollback or owner-wide recovery journal
is used. Read-only EXEC error propagation is separately guarded: GETLK must
not publish a conflict unless the watched read-only EXEC was confirmed.

**Outstanding before Ready/Merge:** complete latest full PR CI, resolve
independent review findings, and keep this document aligned with any further
correctness changes. The implementation does not activate end-to-end FUSE
lock semantics, blocking waits, or final close/release transport: these
remain #431–#433.
