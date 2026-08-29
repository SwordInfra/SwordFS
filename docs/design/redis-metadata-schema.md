# Redis Metadata Schema and Access-Pattern Review

Issue: #109
Scope: Redis Metadata V1, Phase 1

This document is the working design baseline for Redis Metadata V1. It captures decisions made during review and records items that still need validation. Follow-up implementation PRs should use this document as the starting point and update it when implementation reveals a necessary design change.

## 1. Design principles

- Redis and Memory implement the same `IMetaEngine` semantics.
- Redis data structures are selected from filesystem access patterns rather than mechanically mirroring in-memory containers.
- Keys belonging to one volume use the same Redis Cluster hash tag so a metadata transaction stays in one slot.
- Inode metadata is canonical in the inode record.
- A directory maps a name to the child's inode ID and file type. This is the namespace representation needed by `ReadDir`.
- Redis mode must not use `volume.json` as a metadata source.
- Correctness comes before caching, Lua dependencies, batching, and other optimizations.

## 2. Current schema baseline

All keys are scoped by the volume hash tag.

| Key | Redis type | Current purpose | Status |
|---|---|---|---|
| `{db:volume}:format` | String | metadata format/version and volume metadata | Confirmed |
| `{db:volume}:next_ino` | String/integer | inode allocation state | Confirmed |
| `{db:volume}:inode_count` | String/integer | live inode count maintained with inode lifecycle; `StatFs` exposure remains separate | Confirmed lifecycle |
| `{db:volume}:inode:<ino>` | String | canonical serialized `SwordFsInode` | Confirmed |
| `{db:volume}:dir:<parent_ino>` | Hash | `name -> {type, ino}` | Confirmed |
| `{db:volume}:chunk:<ino>` | Hash | `index -> SwordFsChunk` | Current proposal |

`kEntry` remains the single directory-entry record type. Its logical content is `{name, type, ino}`. In a Redis directory Hash, `name` is already the Hash field, so the stored value only needs `{type, ino}`. This is a storage representation of `kEntry`, not a second `kDirEntry` record type.

## 3. Access-pattern matrix — working baseline

| Operation | Reads | Writes | Transaction | Status |
|---|---|---|---|---|
| `Lookup(parent,name)` | `dir:parent`, `inode:child` | — | no | Confirmed pattern |
| `GetInode(ino)` | `inode:ino` | — | no | Confirmed pattern |
| `ReadDir(ino)` | `dir:ino`, possibly directory inode | — / atime if required | iterator state is per open directory handle | Confirmed |
| `OpenDirIterator(ino)` | `inode:ino`, `dir:ino` as required | — / atime handled by `OpenDir` | no long-lived metadata transaction | Confirmed |
| `Readlink(ino)` | `inode:ino` | — | no | Confirmed pattern |
| `FindChunk(ino,idx)` | `chunk:ino` | — | no | Confirmed pattern |
| `ListChunks(ino)` | `chunk:ino` | — | no | Current proposal: incremental hash enumeration |
| `StatFs()` | volume counters | — | no | Counter requirements need validation |
| `Create(parent,name)` | parent inode, `dir:parent`, allocator | inode, dir, parent inode, counters | yes | Confirmed |
| `MkDir(parent,name)` | parent inode, `dir:parent` | inode, new dir, parent inode, counters | yes | Confirmed |
| `Symlink(parent,name)` | parent inode, `dir:parent` | inode, dir, parent inode, counters | yes | Confirmed |
| `Link(ino,parent,name)` | source inode, parent inode, `dir:parent` | inode nlink, dir, parent inode | yes | Confirmed |
| `SetAttr(ino,...)` | inode, chunk Hash when size changes | inode, affected chunk Hash | yes | Confirmed |
| `AddChunk(ino,chunk)` | inode/chunk key as required | chunk Hash | yes | Confirmed |
| `Truncate(ino,size)` | inode, chunk Hash | inode, chunk Hash | yes | Confirmed |
| `Unlink(parent,name)` | parent inode, dentry, child inode | dentry, child inode, parent inode, possibly counters | yes | Confirmed |

| `RmDir(parent,name)` | parent inode, dentry, target dir | dentry, target inode, parent inode, counters | yes | Confirmed |

| `Rename(...)` | source/destination parents, source dentry, optional target dentry/inodes | source/destination dirs, moved inode, optional victim, parent state | yes | Confirmed |
| `ReclaimInode(ino)` | inode, chunk Hash | inode, chunk Hash, counters | yes | Confirmed |


This matrix is a working baseline, not a claim that all transaction details are finalized.

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

This maps directly to the current SwordFS chunk model. We do not copy JuiceFS's slice-list representation without a demonstrated access-pattern benefit.

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

`Unlink` removes one namespace link. It never deletes the inode. The child inode's `nlink` is decremented and its `ctime` is updated; when `nlink` reaches zero, the inode becomes an orphan and remains available until its last open reference is released. The parent directory's `mtime` and `ctime` are updated.

```text
dir:<parent>           HDEL name
inode:<parent>         mtime/ctime updated
inode:<child>          nlink -= 1, ctime = now
```

When `nlink == 1` before unlink, `inode:<child>` is retained with `nlink == 0` until VFS confirms there are no open references and performs reclamation. `Unlink` does not perform that deletion.

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

For replacement of an existing regular file, the destination inode loses one namespace link. If this makes `nlink == 0`, the inode remains in metadata until VFS performs open-fd-aware reclamation; otherwise its `nlink` is decremented and its `ctime` is updated. The source entry is replaced atomically by the destination entry. The overwritten inode and post-operation `nlink` are returned to VFS so it can use the same reclamation path as `Unlink`.

For replacement of an existing directory, the destination must be empty. The source directory replaces the destination entry, the destination directory inode is deleted, and parent-directory link counts are adjusted according to POSIX semantics. In particular, when source and destination parents differ, the source parent loses one child-directory link while the destination parent's child-directory count is unchanged because one directory is replaced by another.

The Redis implementation covers the defined Rename variants while keeping all namespace and link-count changes atomic. Directory cycle prevention and sticky-bit checks are performed before mutation; backend-specific iterator state is not exposed through the metadata API.

### Directory pagination / ReadDir

Large directories are streamed instead of materialized in one operation. The implemented model is:

```text
FUSE fh
  -> SwordFS DirHandle
       -> logical FUSE directory offset
       -> Redis HSCAN cursor
```

The Redis `HSCAN` cursor is private state of the directory handle. Each `HSCAN` returns the cursor for the next scan, and the same open directory handle retains it for subsequent reads. The FUSE `off` is a logical directory position and is **not** the Redis cursor and is not directly converted into one. If a caller seeks to a non-sequential logical offset, the iterator may restart from cursor zero and scan forward until that logical position.

The VFS directory handle owns a backend-neutral `IDirIterator`; Redis stores an `HSCAN` cursor behind that interface, while Memory stores its own iteration state. This keeps backend-specific cursors out of `IMetaEngine` callers. The VFS directory handle serializes `Peek` + `Read` for a given FUSE `fh`, while the Redis iterator also protects its backend state. V1 does not promise a snapshot across concurrent directory mutations; iteration remains best-effort, consistent with the underlying backend's enumeration semantics. Redis directory iterators retain shared ownership of the metadata client so an outstanding handle cannot outlive the Redis client it uses.

### Inode reclamation and chunk lifecycle

`nlink == 0` is a namespace state, not permission for the metadata engine to immediately destroy a regular-file inode. POSIX open-unlink semantics require an unlinked file to remain accessible through an existing file descriptor until the last reference is closed.

The V1 lifecycle is:

```text
namespace link removed
        |
        v
nlink == 0
        |
        +---- open fd exists ----> orphaned inode
        |                              |
        |                              v
        |                         last Close()
        |                              |
        +---- no open fd --------------+
                                       |
                                       v
                              ReclaimData()
                               /          \
                              v            v
                     delete data chunks   ReclaimInode()
                                           |
                                           v
                                      delete inode
                                      delete chunk metadata
```

`ReclaimData` is a VFS-level coordinator because data objects and metadata live in different engines. It enumerates the inode's metadata chunks, deletes the corresponding data-engine objects, and then calls metadata `ReclaimInode`. `ReclaimInode` must be idempotent and must only remove an inode whose `nlink == 0`; if a concurrent `Link` restores a positive link count, reclamation must not delete live data or metadata.

Redis cannot make the Redis metadata mutation and object-store deletion one atomic transaction. Cleanup therefore needs explicit retry/idempotency behavior. A failed data-object deletion must not cause the metadata inode to be deleted prematurely.

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

The directory entry contains `type` and `ino`, which is sufficient to identify the entry. `Lookup` may still need `inode:<ino>` because the API returns the complete `SwordFsInode`. Confirm this against the actual interface and FUSE call path before optimizing away the read.

### ReadDir validation notes

The goal is to avoid N+1 inode reads. The directory Hash already contains the child type and inode ID, so the iterator does not need to fetch every child inode merely to enumerate names.

Pagination/streaming is implemented through a backend-neutral iterator associated with the VFS directory handle:

```text
FUSE fh
  -> FileHandleManager directory handle
       -> logical FUSE directory offset
       -> IDirIterator
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
 nlink == 0 ? orphan inode : normal inode
        ↓
last open reference released
        ↓
   ReclaimInode
        ↓
remove inode/chunk metadata
```

`ReclaimInode` must therefore verify the final reclamation condition before deleting metadata. The Redis implementation must preserve this lifecycle for `Unlink` and Rename-over-file.

### Chunk enumeration

Validate whether Hash field ordering is sufficient for every caller. If `ListChunks` requires ordered output, enumeration can collect entries and sort by chunk index without introducing a second index.

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
3. Validate `ListChunks` ordering requirements against actual callers.
4. Keep Memory and Redis semantic tests aligned as new operations are added.

These access-pattern decisions are now the baseline for the current Redis implementation; future changes should update this document when implementation or measurements require a semantic change.
