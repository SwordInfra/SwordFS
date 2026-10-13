# Redis FileMetadata schema and transaction semantics

This is the current SwordFS beta schema after the typed ChunkID authority
cutover and removal of the common COW descriptor path. It does not promise
compatibility with records from pre-cutover beta versions.

## Keys and authorities

Every key is prefixed with the Redis Cluster hash tag `{db:volume}`. Multi-key
FileMetadata operations stay in one transaction slot; the current single-slot
layout is not horizontal volume sharding.

| Suffix | Type | Authority |
| --- | --- | --- |
| `format` | String | Serialized volume configuration with fixed chunk type and chunk size |
| `next_ino` | Integer string | Monotonic inode allocator |
| `next_chunk_id` | Integer string | Monotonic nonzero volume-scoped `ChunkID` allocator |
| `inode_count` | Integer string | Advisory capacity metric; never an authoritative live-inode count |
| `inode:<ino>` | String | `SwordFsInode` (EOF, attributes, namespace links and xattrs) |
| `dir:<parent>` | Hash | Name to serialized child inode/type mapping |
| `chunk_refs:<ino>` | Hash | Canonical decimal 64-bit `ChunkIndex` field to decimal nonzero `ChunkID` value |
| `private_chunk_index:1:head:<chunk-id>` | String | COW-private serialized `COWChunkHead{revision,size}` |
| `private_chunk_index:1:revision:<chunk-id>` | Integer string | COW-private per-ChunkID revision allocator |
| `orphans` | Hash | Unlinked inode ID to orphan candidate marker |
| `reclaims` | Hash | Inode ID to opaque `ReclaimWork` envelope |
| `pending_deletes` | Hash | Opaque candidate ID to `PendingDelete` envelope |

The `private_chunk_index:<mechanism-key>:<hash>` key constructor remains in
`RedisKey` because typed COW uses it for the two private per-ChunkID records.
There are no legacy descriptor fields or transaction-time raw Hash APIs exposed
to the common metadata engine. Only `cow` is selectable; other persisted
mechanism values are unsupported rather than silently coerced to COW.

A missing `dir:<ino>` Hash represents an empty directory if its inode exists.
`chunk_refs:<ino>` may be absent for an empty or all-hole regular file. A
mapping field and value must decode canonically and fit the supported 64-bit
index and nonzero ChunkID ranges. Corruption fails closed; do not reinterpret
invalid values as holes.

## Allocators and COW keys

`next_chunk_id` uses an atomic validated Redis increment. It never reuses an
identity after detach, even on allocator gaps or ambiguous acknowledgement.
The counter is initialized lazily on first allocation and accepts only
`1..INT64_MAX`. Redis returns a command error for negative, malformed or
overflowed persisted values.

Each COW ChunkID has its own validated revision counter and typed head.
Revision 1 for one ChunkID is unrelated to revision 1 for another. The
immutable object key is derived from `(ChunkID,COWChunkRevision)`; inode and
logical offset are not part of its identity. The COW-private head uses
compare-and-swap of both revision and size. Redis may be the backing service
for both domains, but COW head CAS is not one `RedisMetaTxn` with inode EOF.

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
operations to chunk mechanisms; private COW metadata uses its independent
`COWChunkMetadata` implementation.

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
mint or read a COW revision. `FinalizeAttachedWrite` requires the same
currently attached ID and valid EOF precondition before updating inode size
and write timestamps. Stale attachments cause conflicts; idempotent retries
first inspect the authoritative winner.

`CommitShrink` and `CommitGrow` validate a `ChunkSizePlan` against fresh EOF,
boundary ID and full mapping. The transaction changes common EOF and
`chunk_refs` together. COW retained-boundary sanitation is done in
`FileReadWriter`/typed ChunkMetadata, **not** by the common metadata backend.
`SetAttr(kSize)` is rejected at `RedisMetaOps` and `RedisMetaTxn` so direct
callers cannot bypass COW coordination. Metadata-only mode, owner, time and
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
matches the serialized envelope ID. Mechanism-private typed COW payloads are
decoded only by private GC. Queue membership is never delete authority:
physical deletion requires a fresh mapping and/or COW head reachability
check. A failed lookup, malformed work, or failed delete retains the item for
retry and never guesses its object key.

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
