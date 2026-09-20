# Live inode state

SwordFS deliberately separates write acceptance from persistence. A successful
ordinary `write(2)` may leave bytes only in the inode-shared `FileReadWriter`
until `flush`, `fsync`, or final close publishes the affected chunks. That
durability boundary must not make already accepted writes temporarily
invisible to the same mount.

## Invariant

For an inode with local runtime state, attributes exposed by the VFS are the
authoritative metadata attributes plus only the transient fields whose current
value is newer than durable metadata. Today the only such field is file size:

```text
visible inode attrs = authoritative metadata attrs
                    + local live file size overlay
```

The overlay never caches or replaces the complete inode. Namespace and
identity fields such as `nlink`, mode, uid, gid, and timestamps continue to
come from the metadata engine. This matters for an unlinked-but-open file: the
same reply can legitimately contain `nlink == 0` from metadata and a larger
`size` from an unflushed local write.

## Ownership and lifetime

`FileReadWriter` is shared by all local file handles for one inode and owns the
live file-size state together with its dirty chunks. The state is not stored in
an individual file descriptor and is not persisted independently.

The live size is a transient lower bound established by a successful write: it
records at least the accepted write end while durable metadata still lags. A
fully successful flush clears that lower bound because authoritative metadata
has caught up. Successful truncate/setattr-size operations also clear it: those
operations persist their logical size synchronously, so retaining an old local
overlay would incorrectly mask a later authoritative size change.

A failed flush keeps the lower bound because the accepted write remains local
and must stay visible. Failed size-changing operations likewise leave the
previous local state unchanged.

When no local handle/runtime object exists, VFS attributes come directly from
metadata.

## Attribute reply paths

The overlay applies to every FUSE reply that can refresh attributes for an
already-open regular inode, not only explicit `GETATTR`:

- `GETATTR` reads the inode through the tracked `FileReadWriter`;
- `SETATTR` is serialized through the same object whenever a local inode
  handle exists, including non-size changes whose reply still contains size;
- `LOOKUP` and hard-link entry replies refresh the inode through the tracked
  object before encoding the returned attributes. A hard-link mutation is
  already committed before this enrichment step; a refresh failure therefore
  preserves the successful `Link` result instead of reporting the namespace
  mutation as failed and encouraging an unsafe retry.

The entry paths do not add another metadata lookup for ordinary inodes that
have no local runtime state. New-file `CREATE`, new-directory, and new-symlink
replies have no pre-existing dirty writer state to merge.

`READDIRPLUS` is not covered by this mechanism yet. Its current directory
iterator exposes only name/type/inode, so the plus encoder cannot construct a
complete authoritative inode attribute record at all; it currently leaves
fields such as size and nlink at their default values. Fixing that broader
READDIRPLUS metadata contract, including live-state composition without
accidental per-entry round-trip amplification, is tracked by #229.

## Concurrency

The inode `FileReadWriter::operation_mutex_` is the serialization boundary for
live data and live size. Attribute composition must hold a shared operation
lock while reading the authoritative inode and applying the live-size overlay.
Writes and size-changing operations hold the exclusive side of that same lock.

This ordering is required for shrink correctness. Reading metadata first and
merging the live size later without the common lock can race a truncate:

```text
GetAttr reads old metadata size = 100
truncate commits size = 50
GetAttr overlays against its stale metadata snapshot
```

The common lock gives `GetAttr`, write, and truncate a per-inode linearization
order instead of trying to repair stale snapshots after the fact.

## Read and durability interaction

Dirty reads continue to use the local chunk/write-buffer path. `GetAttr` must
not flush data, and reads must not be forced through persistence merely to make
the file length current. The publication contract remains:

```text
write accepted locally -> live data + live size visible
flush/fsync/final close -> object + chunk metadata + durable inode size
```

The live-size overlay is therefore a visibility mechanism, not a second
durability mechanism.

## Reference design

JuiceFS uses the same high-level split: its inode writer tracks a client-side
length, and attribute replies merge that length over metadata while separately
handling open-unlink lifetime. SwordFS adopts that semantic separation but
keeps its existing dirty-read path; it does not flush pending data before local
read or `GetAttr`.

## Non-goals

- no synchronous flush from `GetAttr` or unlink;
- no per-write persistent inode-size update;
- no full inode-attribute cache;
- no change to durable orphan/reclaimer ownership;
- no general transient timestamp/setid cache in this design.
