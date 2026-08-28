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
| `{db:volume}:inode_count` | String/integer | live inode count, if required by final `StatFs` semantics | Needs validation |
| `{db:volume}:inode:<ino>` | String | canonical serialized `SwordFsInode` | Confirmed |
| `{db:volume}:dir:<parent_ino>` | Hash | `name -> {type, ino}` | Confirmed |
| `{db:volume}:chunk:<ino>` | Hash | `index -> SwordFsChunk` | Current proposal |

`kEntry` remains the single directory-entry record type. Its logical content is `{name, type, ino}`. In a Redis directory Hash, `name` is already the Hash field, so the stored value only needs `{type, ino}`. This is a storage representation of `kEntry`, not a second `kDirEntry` record type.

## 3. Access-pattern matrix — working baseline

| Operation | Reads | Writes | Transaction | Status |
|---|---|---|---|---|
| `Lookup(parent,name)` | `dir:parent`, `inode:child` | — | no | Confirmed pattern |
| `GetInode(ino)` | `inode:ino` | — | no | Confirmed pattern |
| `ReadDir(ino)` | `dir:ino`, possibly directory inode | — / atime if required | TBD | Needs semantic validation |
| `Readlink(ino)` | `inode:ino` | — | no | Confirmed pattern |
| `FindChunk(ino,idx)` | `chunk:ino` | — | no | Confirmed pattern |
| `ListChunks(ino)` | `chunk:ino` | — | no | Current proposal: incremental hash enumeration |
| `StatFs()` | volume counters | — | no | Counter requirements need validation |
| `Create(parent,name)` | parent inode, `dir:parent`, allocator | inode, dir, parent inode, counters | yes | Transaction set needs implementation validation |
| `MkDir(parent,name)` | parent inode, `dir:parent` | inode, new dir, parent inode, counters | yes | Transaction set needs implementation validation |
| `Symlink(parent,name)` | parent inode, `dir:parent` | inode, dir, parent inode, counters | yes | Transaction set needs implementation validation |
| `Link(ino,parent,name)` | source inode, parent inode, `dir:parent` | inode nlink, dir, parent inode | yes | Transaction set needs implementation validation |
| `SetAttr(ino,...)` | inode | inode | yes | Depends on existing semantics |
| `AddChunk(ino,chunk)` | inode/chunk key as required | chunk Hash | yes | Needs implementation validation |
| `Truncate(ino,size)` | inode, chunk Hash | inode, chunk Hash | yes | Needs implementation validation |
| `Unlink(parent,name)` | parent inode, dentry, child inode | dentry, child inode, parent inode, possibly counters | yes | Needs semantic validation |
| `RmDir(parent,name)` | parent inode, dentry, target dir | dentry, target inode, parent inode, counters | yes | Needs semantic validation |
| `Rename(...)` | source/destination parents, source dentry, optional target dentry/inodes | source/destination dirs, moved inode, optional victim, parent state | yes | **Needs detailed review** |
| `ReclaimInode(ino)` | inode, chunk Hash | inode, chunk Hash, counters | yes | **Needs detailed review** |

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

## 5. Items still under design review

### `inode_count`

Determine exactly which `StatFs` fields are required and whether a persistent counter is necessary. If it is required, creation and final inode reclamation must update it atomically with the corresponding inode lifecycle operation.

### Lookup

The directory entry contains `type` and `ino`, which is sufficient to identify the entry. `Lookup` may still need `inode:<ino>` because the API returns the complete `SwordFsInode`. Confirm this against the actual interface and FUSE call path before optimizing away the read.

### ReadDir

The goal is to avoid N+1 inode reads. Validate exactly which attributes `ReadDir` must return and whether directory inode metadata itself needs to be loaded for current semantics.

### Rename

Review all existing flags and Memory semantics before fixing the transaction set. In particular:

- same-directory rename
- cross-directory rename
- replace existing file
- replace empty directory
- no-replace
- exchange, if exposed by SwordFS
- directory parent link-count changes
- cycle prevention

The transaction must keep all namespace and link-count changes atomic.

### Unlink / RmDir / Reclaim

Confirm the exact lifecycle between `nlink == 0`, open references, and physical inode/chunk reclamation. Do not assume that deleting an inode from Redis immediately is equivalent to unlinking it.

### Chunk enumeration

Validate whether Hash field ordering is sufficient for every caller. If `ListChunks` requires ordered output, enumeration can collect entries and sort by chunk index without introducing a second index.

## 6. JuiceFS comparison

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

## 7. Next review step

Before implementing metadata mutations, complete the detailed read/write and transaction analysis for:

1. `Lookup` / `ReadDir`
2. `Create` / `MkDir` / `Symlink`
3. `Link` / `Unlink` / `RmDir`
4. `Rename`
5. `ReclaimInode` and chunk lifecycle
6. `StatFs` / `inode_count`

Only after these are agreed should the corresponding Redis metadata operations be implemented.
