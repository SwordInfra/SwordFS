# Redis FileMetadata schema and transaction semantics

This describes the current Slice-based SwordFS beta schema. The historical
COW=1 volume identity is reserved and remains decodable for an explicit mount
error; no COW private-index implementation or migration is supported.

## Keys and authorities

Every key is prefixed with the Redis Cluster hash tag `{db:volume}`. Multi-key
FileMetadata operations stay in one transaction slot; the current single-slot
layout is not horizontal volume sharding.

| Suffix | Type | Authority |
| --- | --- | --- |
| `format` | String | Serialized volume configuration with fixed chunk type and chunk size |
| `next_ino` | Integer string | Monotonic inode allocator |
| `next_chunk_id` | Integer string | Monotonic nonzero volume-scoped `ChunkID` allocator |
| `next_slice_id` | Integer string | Monotonic nonzero volume-scoped Slice object allocator |
| `inode_count` | Integer string | Advisory capacity metric; never an authoritative live-inode count |
| `inode:<ino>` | String | `SwordFsInode` (EOF, attributes, namespace links and xattrs) |
| `dir:<parent>` | Hash | Name to serialized child inode/type mapping |
| `chunk_refs:<ino>` | Hash | Canonical decimal 64-bit `ChunkIndex` field to decimal nonzero `ChunkID` value |
| `private_chunk_index:2:index:<chunk-id>` | List | Ordered immutable Slice records for the attached ChunkID |
| `orphans` | Hash | Unlinked inode ID to orphan candidate marker |
| `reclaims` | Hash | Inode ID to opaque `ReclaimWork` envelope |
| `pending_deletes` | Hash | Opaque candidate ID to `PendingDelete` envelope |

The `private_chunk_index:<mechanism-key>:<hash>` constructor is mechanism-neutral;
Slice uses enum value 2. The removed COW private keys are neither read nor
written. Only `chunk_slice` is selectable for newly formatted and mounted
volumes. Historical `cow` and future `redis_cache` both fail closed.

A missing `dir:<ino>` Hash represents an empty directory if its inode exists.
`chunk_refs:<ino>` may be absent for an empty or all-hole regular file. A
mapping field and value must decode canonically and fit the supported 64-bit
index and nonzero ChunkID ranges. Corruption fails closed; do not reinterpret
invalid values as holes.

## Allocators and Slice records

`next_chunk_id` uses an atomic validated Redis increment. It never reuses an
identity after detach, even on allocator gaps or ambiguous acknowledgement.
The counter is initialized lazily on first allocation and accepts only
`1..INT64_MAX`. Redis returns a command error for negative, malformed or
overflowed persisted values.

The Slice object allocator `next_slice_id` is also volume-scoped and
monotonic. Each ChunkID has a private append-only Slice record list identifying
immutable objects and their covered logical ranges. The record type, not the
common Redis transaction layer, interprets Slice object keys and offsets.
Ambiguous allocation or append outcomes cannot be replayed blindly.

## Transaction layers

```text
RedisMetaImpl     IMetaEngine validation and POSIX policy
    |
RedisMetaOps      semantic operations and optimistic retry boundary
    |
RedisMetaTxn      FileMetadata inode, directory, EOF and chunk_refs semantics
    |
RedisKvTxn        WATCH -> MULTI -> EXEC
    |
RedisMetaClient   Redis client on BlockingExecutor POSIX threads
```

Fiber callers suspend while Redis work executes in the POSIX-thread domain.
`RedisMetaTxn` is per-attempt and does not expose raw `Get/Put/Scan/Erase`
operations to chunk mechanisms; private Slice records are maintained through the separate
`RedisSliceChunkMetadata` implementation.

On Redis `EXEC` conflicts, the operation retries from a fresh transaction
snapshot. A command error within EXEC may occur after prior commands already
ran, so **the result is potentially partially applied**. A lost EXEC reply
or network timeout may mean a committed mutation. The caller must reconcile
by reading current FileMetadata, not reconstructing the outcome from local
assumptions or a cleanup queue. New transactions must not replay stale
`ChunkID` attachments, detached lists, or old EOF plans.

## FileMetadata operations

`ReadFileChunkSnapshot` loads one mapping with inode EOF and retained boundary
snapshot. `ReadFileMappingSnapshot` scans the entire authoritative mapping for
size planning and reclaim. `ProbeAttachment` gives GC an authoritative
reachability answer. A missing mapping inside EOF is a sparse hole.

`AttachPrepared(ino,index,ChunkID,end,expected)` validates the attached ID,
file type, current EOF, expected boundary and absent existing mapping under
WATCH, then writes the selected ID and updates EOF as needed. It does not
mint or read a private Slice object identity. `FinalizeAttachedWrite` requires the same
currently attached ID and valid EOF precondition before updating inode size
and write timestamps. Stale attachments cause conflicts; idempotent retries
first inspect the authoritative winner.

`CommitShrink` and `CommitGrow` validate a `ChunkSizePlan` against fresh EOF,
boundary ID and full mapping. The transaction changes common EOF and
`chunk_refs` together. Retained-boundary Slice sanitation is coordinated by
`FileReadWriter` and `SliceChunk`, **not** the common metadata backend.
`SetAttr(kSize)` is rejected at `RedisMetaOps` and `RedisMetaTxn` so direct
callers cannot bypass Slice/EOF coordination. Metadata-only mode, owner, time and
xattr changes continue through the ordinary transactional setters.

## Reclaim and cleanup

Unlink records an orphan candidate on last-link removal, but does not delete
an inode while its local open descriptor fence remains. `PrepareReclaim`
reads the current inode and, for a regular file, WATCHes its typed
`chunk_refs` mapping. A linked/revived inode is not reclaimable.

For a last-link inode, the destructive transaction orders `DEL inode:<ino>`
**before** `DEL chunk_refs:<ino>`; both commands are type-insensitive. It
also removes orphan markers and may queue an opaque private `ReclaimWork`.
The pure `ChunkCleanupWorkEncoder` freezes only the detached `ChunkID`
identities. For a special inode or empty file, the payload is a valid empty
private reclaim record. Deleting the inode is the non-revivability point of
no return: Link and reclaim serialize on the same watched inode key.

Because Redis EXEC can be partially applied, an auxiliary cleanup-registration
error after the inode deletion may leave unreachable residue or lost work;
that is a possible space leak, **not** permission to delete live data. An
absent inode is already logically reclaimed. A surviving inode with `nlink >
0` is live. For `nlink == 0`, retry from fresh watched mappings. `inode_count`
is not part of this correctness proof.

`reclaims` and `pending_deletes` store opaque envelopes; their Hash field
matches the serialized envelope ID. Slice cleanup payloads are opaque to common FileMetadata. No physical Slice
delete worker is active: queued work is preserved pending #273, which must
first establish private-index reachability and remote reader-lifetime safety.
Queue membership is never authority to delete an object.

`StatFs` uses virtual capacity instead of treating a stale or corrupt
`inode_count` as an authoritative link count. Namespace and inode attributes
remain canonical even if that advisory metric is missing or malformed.

## Persistence and testing

Metadata envelopes use the current format schema only. Beta development may
replace incompatible layouts without a migration/dual-read decoder. Redis
integration fixtures isolate test volumes under unique names and validate
optimistic races, typed mapping scan corruption, ambiguous EXEC reconciliation,
empty/sparse reclaim, and GC fail-closed behavior.

See [chunk publication](chunk-publication.md),
[data structures](data-structures.md) and [architecture](architecture.md).
