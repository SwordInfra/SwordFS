# Redis Metadata Schema and Access-Pattern Review

Issue: #109
Scope: Redis Metadata V1, Phase 1

This document is the detailed Redis persistence/access-pattern reference. For the overall SwordFS architecture and cross-subsystem lifecycle, start with [SwordFS Architecture](architecture.md). This document captures Redis-specific representation and transaction decisions and should stay aligned with the current `IMetaEngine` contract.

## 1. Design principles

- Redis and Memory implement the same `IMetaEngine` semantics.
- Redis data structures are selected from filesystem access patterns rather than mechanically mirroring in-memory containers.
- Keys belonging to one volume use the same Redis Cluster hash tag so a metadata transaction stays in one slot.
- Inode metadata is canonical in the inode record.
- A directory maps a name to the child's inode ID and file type. This is the namespace representation needed by `ReadDir`.
- Redis mode persists volume configuration in Redis rather than using the local `volume.fmt` file used by the Memory backend.
- Correctness comes before caching, Lua dependencies, batching, and other optimizations.

## 2. Current schema baseline

All keys are scoped by the volume hash tag.

| Key | Redis type | Current purpose | Status |
|---|---|---|---|
| `{db:volume}:format` | String | metadata format/version and volume metadata | Confirmed |
| `{db:volume}:next_ino` | String/integer | inode allocation state | Confirmed |
| `{db:volume}:next_chunk_revision` | String/integer | globally monotonic immutable chunk-revision allocator | Confirmed |
| `{db:volume}:inode_count` | String/integer | live inode count maintained with inode lifecycle; `StatFs` exposure remains separate | Confirmed lifecycle |
| `{db:volume}:inode:<ino>` | String | canonical serialized `SwordFsInode` | Confirmed |
| `{db:volume}:dir:<parent_ino>` | Hash | `name -> {type, ino}` | Confirmed |
| `{db:volume}:chunk:<ino>` | Hash | `index -> SwordFsChunk` | Confirmed |
| `{db:volume}:orphans` | Hash | `ino -> marker` for durable last-link orphan candidates | Confirmed |
| `{db:volume}:reclaims` | Hash | `ino -> serialized ReclaimWork` after reclaim point of no return | Confirmed |

`kEntry` remains the single directory-entry record type. Its logical content is `{name, type, ino}`. In a Redis directory Hash, `name` is already the Hash field, so the stored value only needs `{type, ino}`. This is a storage representation of `kEntry`, not a second `kDirEntry` record type.

## 3. Access-pattern matrix

| Operation | Reads | Writes | Transaction | Status |
|---|---|---|---|---|
| `Lookup(parent,name)` | `dir:parent`, `inode:child` | — | no | Confirmed pattern |
| `GetInode(ino)` | `inode:ino` | — | no | Confirmed pattern |
| `ReadDir(ino, offset, max_entries, iterator)` | `dir:ino`, directory inode on first call | — / best-effort atime on first call | iterator state is owned by the caller and reused across calls | Confirmed |
| `Readlink(ino)` | `inode:ino` | — | no | Confirmed pattern |
| `FindChunk(ino,idx)` | `chunk:ino` | — | no | Confirmed pattern |
| `VisitChunks(ino,visitor)` | `chunk:ino` | — | no | Confirmed incremental enumeration |
| `StatFs()` | volume counters | — | no | Counter requirements need validation |
| `Create(parent,name)` | parent inode, `dir:parent`, allocator | inode, dir, parent inode, counters | yes | Confirmed |
| `MkDir(parent,name)` | parent inode, `dir:parent` | inode, new dir, parent inode, counters | yes | Confirmed |
| `Symlink(parent,name)` | parent inode, `dir:parent` | inode, dir, parent inode, counters | yes | Confirmed |
| `Link(ino,parent,name)` | source inode, parent inode, `dir:parent` | inode nlink, dir, parent inode | yes | Confirmed |
| `SetAttr(ino,...)` | inode, chunk Hash when size changes | inode, affected chunk Hash | yes | Confirmed |
| `CommitChunk(ino,expected,replacement)` | inode, `chunk:ino` | inode/chunk Hash | yes | Confirmed CAS/idempotent publication |
| `Truncate(ino,size)` | inode, chunk Hash | inode, chunk Hash | yes | Confirmed |
| `Unlink(parent,name)` | parent inode, dentry, child inode | dentry, child inode, parent inode, possibly counters | yes | Confirmed |

| `RmDir(parent,name)` | parent inode, dentry, target dir | dentry, target inode, parent inode, counters | yes | Confirmed |

| `Rename(...)` | source/destination parents, source dentry, optional target dentry/inodes | source/destination dirs, moved inode, optional victim, parent state | yes | Confirmed |
| `PrepareReclaim(ino)` | pending reclaim, inode, chunk Hash | pending reclaim, orphan marker, inode, chunk Hash, counters | yes | Confirmed point-of-no-return transition |
| `CompleteReclaim(ino)` | pending reclaim | pending reclaim | yes | Confirmed idempotent completion |


This matrix summarizes the current Redis access pattern. Transaction mechanics may continue to evolve as long as the `IMetaEngine` semantics remain unchanged.

Access-time updates are best-effort metadata side effects. An access operation must not fail because its atime update conflicts with another metadata mutation or otherwise cannot be persisted. Redis may use a separate optimistic transaction for the atime update, but failure of that transaction is logged and does not change the result of the enclosing filesystem operation. Read-only metadata access itself must not be placed inside a WATCH/MULTI/EXEC transaction merely because atime is updated as a side effect.

## 4. Confirmed design choices

### Directory representation

Use one Redis Hash per directory:

```text
{db:volume}:dir:<parent_ino>
    name -> {type, ino}
```

This follows the useful part of the JuiceFS design: directory enumeration can obtain the child inode ID and file type without an inode lookup for every entry. Full inode attributes remain canonical in `inode:<ino>`.

There is no separate `kDirEntry` record type.

### Chunk representation

Use one Redis Hash per inode:

```text
{db:volume}:chunk:<ino>
    chunk_index -> SwordFsChunk
```

`SwordFsChunk` stores logical chunk identity and the currently authoritative immutable revision, not a physical object-storage key:

```text
SwordFsChunk {
    index
    start_offset
    revision      // uint64, 0 is invalid
    size
}
```

Object-storage keys are derived by the data path from `(ino, chunk_index, revision)`. A physical object key is therefore not part of the metadata schema. New writes and rewrites use the same publication model:

```text
revision = INCR {db:volume}:next_chunk_revision
PUT object(ino, chunk_index, revision)
CommitChunk(expected?, replacement)
```

Revision allocation and authoritative publication are deliberately separate. A failed operation may consume a revision ID or leave an unreachable object, but it does not publish pending state into the chunk Hash. Revision IDs are unique and monotonically increasing within the volume; gaps are valid. A rewrite must publish a revision greater than the descriptor it replaces.

`next_chunk_revision` is part of durable metadata state and must have the same persistence and no-eviction guarantees as inode and chunk metadata. Losing or rolling back the counter could reuse a physical object identity and violate immutable-publication semantics. The object-key namespace also assumes a SwordFS volume owns an isolated bucket/prefix namespace (or another equivalent volume discriminator is present outside the derived key).

This maps directly to the current SwordFS whole-chunk model. We do not copy JuiceFS's slice-list representation without a demonstrated access-pattern benefit, but we follow the same useful principle that immutable data identity is separate from metadata publication.

### Volume locality

All metadata keys for a volume use the same Redis Cluster hash tag. This is a prerequisite for transactions touching multiple metadata keys in that volume.

## 5. Metadata mutation semantics

The mutation semantics below are defined from POSIX filesystem semantics. JuiceFS is used as an implementation reference, but Redis schema and access patterns should be derived from the semantics and SwordFS operation set rather than copied mechanically.

### Create

```text
new_ino = INCR {volume}:next_ino
```

The inode allocator uses Redis's native atomic `INCR`; inode numbers do not need to be contiguous, so an inode ID may be consumed even when the subsequent Create transaction conflicts or fails.

The namespace mutation must atomically create the child inode, add the parent directory entry, and update the parent directory timestamps. The parent inode and parent directory are watched because they are both part of the mutation's state.

Planned write semantics:

```text
inode:<new_ino>          create regular-file inode
 dir:<parent>            name -> {new_ino, REG}
inode:<parent>           mtime = now, ctime = now
```

Redis Lua is deliberately not part of V1. The initial implementation should use the existing Redis transaction abstraction with `WATCH` + `MULTI/EXEC`; Lua can be considered later as a performance optimization.

### MkDir

`MkDir` follows the same inode allocation and atomic namespace mutation model as `Create`, with directory-specific link-count semantics:

```text
new directory inode: nlink = 2       // "." and ".."
parent inode:         nlink += 1
parent inode:         mtime/ctime updated
parent directory:     name -> {new_ino, DIR}
```

An empty directory does not require an explicitly created empty `dir:<new_ino>` Hash. A missing directory Hash represents an empty directory.

### Link

A hard link adds a directory entry and increments the source inode's link count. The source inode's `ctime` changes; the target parent directory's `mtime` and `ctime` change.

```text
dir:<parent>           name -> {source_ino, source_type}
inode:<source>          nlink += 1, ctime = now
inode:<parent>          mtime/ctime updated
```

The source inode, target parent inode, and target parent directory are part of the mutation state. No inode-to-parent reverse index is required.

### Unlink

`Unlink` removes one namespace link. It never deletes object data. The child inode's `nlink` is decremented and its `ctime` is updated; when `nlink` reaches zero, the same transaction publishes the inode into the durable `orphans` Hash. The parent directory's `mtime` and `ctime` are updated.

```text
dir:<parent>           HDEL name
inode:<parent>         mtime/ctime updated
inode:<child>          nlink -= 1, ctime = now
orphans                HSET child_ino marker  (when nlink reaches 0)
```

When `nlink == 1` before unlink, `inode:<child>` is retained with `nlink == 0` and the orphan marker becomes the durable cleanup authority. The background reclaimer later checks the local open-reference fence and attempts `PrepareReclaim`; foreground unlink does not delete the inode or its data objects.

### RmDir

`RmDir` requires the target to be a directory and empty. Directory emptiness is represented by the absence of entries in `dir:<child>`; Redis `HLEN` is sufficient to check this without enumerating the directory.

On success:

```text
dir:<parent>           HDEL name
inode:<parent>         nlink -= 1, mtime/ctime updated
dir:<child>            DEL
inode:<child>          DEL
```

`dir:<child>` must be part of the transaction's watched state so a concurrent child creation cannot race with the emptiness check. V1 deletes the target directory inode immediately; there is no deferred reclaim semantics for directories at this stage.

### Rename

Rename semantics follow POSIX. The operation is atomic across all affected namespace and inode metadata.

For a destination that does not exist:

```text
dir:<src_parent>       HDEL src_name
dir:<dst_parent>       HSET dst_name {src_ino, src_type}
inode:<src_parent>     mtime/ctime updated
inode:<dst_parent>     mtime/ctime updated
inode:<src_ino>        ctime updated
```

For replacement of an existing regular file, the destination inode loses one namespace link. If this makes `nlink == 0`, the same transaction publishes a durable orphan marker; otherwise its `nlink` is decremented and its `ctime` is updated. The source entry is replaced atomically by the destination entry. VFS does not need a post-rename link-count result for reclamation; metadata owns publication of the durable orphan candidate.

For replacement of an existing directory, the destination must be empty. The source directory replaces the destination entry, the destination directory inode is deleted, and parent-directory link counts are adjusted according to POSIX semantics. In particular, when source and destination parents differ, the source parent loses one child-directory link while the destination parent's child-directory count is unchanged because one directory is replaced by another.

The Redis implementation covers the defined Rename variants while keeping all namespace and link-count changes atomic. Directory cycle prevention and sticky-bit checks are performed before mutation; backend-specific iterator state is not exposed through the metadata API.

### Directory pagination / ReadDir

`ReadDir` is the single directory enumeration API. The caller owns a nullable
`DirIterator`: on the first call it is null and the metadata engine creates the
backend-specific iterator; subsequent calls pass the same iterator to continue
the enumeration. The iterator may encapsulate a Redis `HSCAN` cursor or an
in-memory snapshot. The `offset` is the logical FUSE directory position and
`max_entries` limits the returned batch.

Large directories are streamed instead of materialized in one operation. The implemented model is:

```text
FUSE fh
  -> SwordFS DirHandle
       -> logical FUSE directory offset
       -> Redis HSCAN cursor
```

The Redis `HSCAN` cursor is private state of the directory handle. Each `HSCAN` returns the cursor for the next scan, and the same open directory handle retains it for subsequent reads. The FUSE `off` is a logical directory position and is **not** the Redis cursor and is not directly converted into one. If a caller seeks to a non-sequential logical offset, the iterator may restart from cursor zero and scan forward until that logical position.

The VFS directory handle owns a backend-neutral `DirIterator`; Redis stores an `HSCAN` cursor behind that interface, while Memory stores its own iteration state. This keeps backend-specific cursors out of `IMetaEngine` callers. The VFS directory handle serializes `Peek` + `Advance` for a given FUSE `fh`, while the Redis iterator also protects its backend state. V1 does not promise a snapshot across concurrent directory mutations; iteration remains best-effort, consistent with the underlying backend's enumeration semantics. Redis directory iterators retain shared ownership of the metadata backend context so an outstanding handle cannot outlive the Redis client/executor resources it uses.

### Inode reclamation and chunk lifecycle

`nlink == 0` is a namespace state, not permission to immediately destroy a regular-file inode or its data. POSIX open-unlink semantics require an unlinked file to remain accessible through an existing file descriptor until the local open-reference fence is clear.

The current durable lifecycle is:

```text
namespace link removed
        |
        v
nlink == 0 + durable orphan marker
        |
        +---- local open reference exists ----> keep orphan marker; retry later
        |
        v
PrepareReclaim()
        |
        | atomically freeze authoritative object identities,
        | write reclaims[ino], remove orphan marker/live inode/chunk map
        v
durable pending reclaim
        |
        +---- object delete fails ----> keep pending record; retry/restart safe
        |
        v
delete all frozen object identities
        |
        v
CompleteReclaim()
        |
        v
remove pending record
```

The VFS foreground path does not execute this deletion sequence. The background `Reclaimer` is the cross-engine coordinator. Before `PrepareReclaim`, it acquires the local `InodeHandle` reclaim fence so an open/opening descriptor cannot race the metadata point of no return.

`PrepareReclaim` rechecks `nlink == 0`, freezes the authoritative chunk descriptors and their immutable object keys into `reclaims[ino]`, removes the orphan marker, and removes live inode/chunk metadata atomically. A concurrent `Link` that wins first makes preparation a no-op and clears stale orphan state; once preparation succeeds, the live inode no longer exists and object deletion can proceed without the local fence.

The reclaimer deletes every frozen object idempotently and calls `CompleteReclaim` only after all deletes succeed. `CompleteReclaim` removes the pending record idempotently. This makes crash/restart and object-delete failure recoverable without reconstructing delete targets from mutable metadata.

Redis cannot make the Redis metadata mutation and object-store deletion one atomic transaction. The durable pending-reclaim record is therefore the handoff between those systems: live metadata is removed only together with publication of the frozen work, and the frozen record is retained until object deletion completes.

For directories, `RmDir` is different: the target must be empty and directories have no data chunks, so target directory metadata can be removed as part of the atomic namespace mutation.

### Inode-to-parent reverse index

Do not introduce an `inode -> parents` reverse index in V1. The current SwordFS operations can be implemented using the forward directory index:

```text
dir:<parent> -> name -> {ino, type}
```

Hard links do not require discovering all parents because link count is maintained on the inode itself. A reverse index should only be introduced if a concrete future operation demonstrates a correctness or performance requirement for reverse parent lookup.

## 6. Items still requiring follow-up validation

### `inode_count`

A persistent `inode_count` is used by the current Redis implementation as the live-inode counter. Creation increments it and final inode reclamation decrements it atomically with the corresponding metadata lifecycle operation. The remaining question is whether `StatFs` should expose this counter directly; that validation is separate from the lifecycle invariant.

### Lookup

The directory entry contains `type` and `ino`, which is sufficient to identify the entry. `Lookup` still reads `inode:<ino>` because the current API returns the complete `SwordFsInode`, not only the directory-entry tuple.

### ReadDir validation notes

The goal is to avoid N+1 inode reads. The directory Hash already contains the child type and inode ID, so the iterator does not need to fetch every child inode merely to enumerate names.

Pagination/streaming is implemented through a backend-neutral iterator associated with the VFS directory handle:

```text
FUSE fh
  -> HandleManager / DirHandle
       -> logical FUSE directory offset
       -> DirIterator
            ├── Memory: backend-neutral in-memory state
            └── Redis: private HSCAN cursor + prefetched entries
```

The FUSE `off` is a logical directory position. It must not be treated as, or directly mapped to, a Redis cursor. VFS first peeks the next entry to determine whether it fits the byte-sized FUSE buffer, then consumes it only after successful encoding. Sequential reads reuse the iterator's backend state. A non-sequential seek restarts the iterator and scans forward to the requested logical position. Seeking past the current end returns an empty, end-of-directory result rather than `EINVAL`. V1 does not provide snapshot isolation across concurrent directory mutations.

### Rename

The current implementation has been aligned with the Memory semantics for:

- same-directory rename
- cross-directory rename
- replace existing file
- replace empty directory
- no-replace
- exchange
- directory parent link-count changes
- cycle prevention

The transaction keeps all namespace and link-count changes atomic. Future changes should extend both engines' tests together.

### Unlink / RmDir / Reclaim

The regular-file lifecycle is defined by the POSIX open-unlink requirement: namespace removal and final inode reclamation are separate operations.

```text
namespace link removed
        ↓
      nlink--
        ↓
 nlink == 0 ? publish durable orphan : normal inode
        ↓
background worker + local reclaim fence
        ↓
  PrepareReclaim
        ↓
freeze immutable object identities and remove live inode/chunk metadata
        ↓
delete frozen data objects
        ↓
CompleteReclaim
```

`PrepareReclaim` is the metadata point of no return and must verify the final reclamation condition before removing live metadata. The Redis implementation must preserve this lifecycle for `Unlink` and rename-over-file, and `Link` must atomically revive/clear an orphan candidate when it wins before preparation.

### Chunk enumeration

`VisitChunks` incrementally enumerates the per-inode chunk Hash and does not expose Redis cursor state through `IMetaEngine`. The interface does not promise sorted output; callers that require ordering must establish it explicitly rather than depending on Redis Hash field order.

## 7. JuiceFS comparison

### Adopted

- volume-scoped Redis namespace/hash tag for transaction locality
- inode as an independent metadata object
- directory as a Hash keyed by parent inode
- directory values carry child inode ID and file type, simplifying `ReadDir`
- explicit schema/version discipline

### Deliberately different

- no inode-to-parent reverse index unless a SwordFS operation proves it necessary
- no JuiceFS slice-list representation for chunks; use the simpler SwordFS chunk model
- no duplicated full inode attributes in directory entries
- no unnecessary Redis-specific semantics leaking into `IMetaEngine`

These are design conclusions, but individual items can be revisited if access-pattern or performance measurements demonstrate a need.

## 8. Next review step

The POSIX semantics for the core namespace mutations and the VFS directory iterator are now recorded. The remaining follow-up work should focus on:

1. Validate `ReadDir` behavior under concurrent directory mutation and arbitrary FUSE seek/restart workloads.
2. Resolve whether `StatFs` should expose the already-confirmed `inode_count` lifecycle counter.
3. Revisit chunk-enumeration ordering only if a future caller requires a stable sorted contract rather than the current visitor semantics.
4. Keep Memory and Redis semantic tests aligned as new operations are added.

These access-pattern decisions are now the baseline for the current Redis implementation; future changes should update this document when implementation or measurements require a semantic change.
