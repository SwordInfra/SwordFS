#531 — Decoupled, read-driven access time

## Requirement and decision

Regular `open(2)` has no data-access side effect. Successful positive-length
regular-file reads update atime on mounts without `noatime`, but atime is
**advisory metadata**: its persistence does not participate in Slice file-size,
mapping, or lifecycle WATCH/EXEC transactions. An atime write must never make
an otherwise successful data read/flush fail. Explicit `SETATTR(atime)` must
remain authoritative and must be allowed to move time backwards.

Peer reference: JuiceFS exposes `noatime` (its default), `relatime`, and
`strictatime`; its documentation notes the metadata transaction cost of atime.
SwordFS retains its existing default-mount semantics while reducing contention
through a separately persisted Redis access-time overlay.

## Redis ownership and update protocol

* The main serialized `inode:<ino>` remains authoritative for permissions,
  size, mtime, ctime, mappings and inode existence; no persistent schema or
  version migration is required in beta.
* A **distinct Redis key** `atime:<ino>` holds the optional regular-file atime
  overlay. Merely changing a Redis Hash field of `inode:<ino>` would not work:
  WATCH monitors the entire key. This key is never watched by Slice publication.
* `RecordReadAccess(ino)` is an advisory operation from the data read path.
  Its best-effort Redis Lua update checks that `inode:<ino>` still equals the
  authoritative record observed by that access, then updates only `atime:<ino>`.
  Concurrent inode metadata changes can safely discard an access update. The
  overlay advances monotonically for implicit reads, but explicit timestamp
  setting may move the value backwards.
* Explicit `SetAttr(kAtime / kAtimeNow)` deletes the overlay in the **same**
  authoritative inode mutation transaction. The main inode contains the
  requested value. In-flight read updates based on older inode snapshots are
  rejected by the Lua compare; later reads may legitimately advance atime.
* At reclaim's point of no return, delete `atime:<ino>` atomically alongside
  the main inode. Inode IDs are nonreused. The Lua update cannot create a
  sidecar after an inode was removed because it checks inode existence.
* Metadata GetInode/GetInodes/Lookup return the overlay's atime when present;
  a missing overlay falls back to the atime in the main inode. The overlay is
  not a source of authority for size or mappings and must never be consulted
  from the transaction's publication paths.

## Performance, ordering and failure semantics

The hot read path opportunistically records access once per inode per bounded
interval (initially 60 seconds), **not** once per read. The update runs on the
existing Redis POSIX executor, never directly in a fiber. Failed advisory
updates are logged but do not change the read's successful result. This first
iteration performs that infrequent update synchronously and intentionally
avoids a new unmanaged background queue or shutdown/lifetime hazards;
background batching is a separate optimization if measurements justify it.

The overlay is Redis-persistent, not a volatile RAM-only update; a lost
acknowledgement may mean one access was recorded or dropped. Future accesses
converge. Default `stat` may lag recent reads by at most the local throttle
interval absent errors, while cross-mount visibility follows Redis reads.
Explicit SETATTR keeps its transactional success contract. `noatime` never
records implicit data access, but explicit user timestamp changes still work.

## Required test-first verification

1. Open does not change atime; successful read records it on normal mounts,
   and disabled `noatime` mode suppresses implicit updates.
2. A real Redis transaction watching the inode stays valid across atime
   updates; Slice publication remains strictly audited by generic/113.
3. Explicit atime backdating clears the overlay; a stale read cannot restore
   it; a new read can advance it. Inode reclaim deletes the sidecar.
4. Cross-client GetInode/GetInodes see the overlay, without changing size,
   mtime, ctime or chunk metadata.
5. Focused RED→GREEN Dev Build, full PR CI and strict writeback audits, then
   independent review. Stack this task on #530; do not change #530's prior
   verified SHA until the dependency is ready.
