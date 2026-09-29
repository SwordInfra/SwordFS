# Persistent extended attributes

Issue #378 introduces the first persistent extended-attribute contract in
SwordFS. This document owns the stable design; the Issue remains the durable
record for investigation, TDD evidence, CI, review, and rollout progress.

## Scope

The first version implements raw `user.*` extended attributes for the Memory
and Redis metadata engines and exposes them through low-level FUSE
`setxattr(2)`, `getxattr(2)`, `listxattr(2)`, and `removexattr(2)` callbacks.

This layer is deliberately only the raw xattr foundation. POSIX ACL parsing,
inheritance, permission evaluation, and `system.posix_acl_*` semantics are not
part of this Issue. `trusted.*` and other namespaces are also not enabled by
this version. A callback exists for these requests, so an unsupported
namespace is reported as `EOPNOTSUPP`, not `ENOSYS`.

## Semantic contract

Each inode owns a mapping from xattr name to opaque bytes. The metadata API
uses a backend-neutral set mode:

- **upsert**: create a missing attribute or replace an existing one;
- **create-only**: fail with `EEXIST` if the name already exists;
- **replace-only**: fail with `ENODATA` if the name does not exist.

The remaining externally visible rules are:

- a missing inode is `ENOENT`;
- a missing xattr is `ENODATA` for get, replace-only set, and remove;
- invalid Linux set flags are `EINVAL` before the metadata boundary;
- a successful set or remove updates inode `ctime` in the same atomic metadata
  mutation as the xattr change;
- get/list are reads and do not update inode timestamps;
- values are opaque binary data and may contain NUL bytes;
- list results have deterministic name order;
- xattrs follow inode identity across rename and hard links;
- removing one hard link does not remove xattrs while another name remains;
- final inode reclaim removes xattrs together with the inode record.

The kernel remains the ordinary DAC authority through the existing
`default_permissions` contract. This feature does not rebuild pathname or
inode permission checks in metadata.

## Metadata authority and persistence

`SwordFsInode` is the single authority for raw xattrs. The mapping is embedded
in the serialized inode record rather than stored in a side table or a
backend-specific Redis key.

This choice keeps lifecycle semantics structural: namespace operations already
preserve or mutate inode identity, and reclaim already deletes the live inode
record. There is therefore no second xattr lifecycle that can drift from
hard-link, rename, orphan, or reclaim state.

The persistent encoding is canonical:

1. encode the xattr count;
2. encode name/value pairs in lexicographic name order;
3. reject duplicate names and malformed/truncated data while decoding;
4. require exact record consumption, as other SwordFS metadata records do.

Adding xattrs changes the inode record layout in place. SwordFS is currently
beta and does not preserve superseded development metadata formats, so this
feature does not introduce a schema-version transition, dual decoder, or
migration path. Existing beta metadata created before this layout change is
not a compatibility target; development volumes may be recreated as needed.
The repository describes only the current format.

For Redis, xattrs remain part of the existing `inode:<ino>` String. A Redis
xattr mutation WATCHes and rewrites that inode record through the same
optimistic transaction machinery used by other inode mutations. There is no
new Redis Hash or key family. The Memory backend mutates the same logical
record while holding its transaction mutex.

## API boundaries

The boundaries intentionally separate semantics from transport details.

### Metadata engine

`IMetaEngine` exposes complete typed operations:

- `SetXAttr(ino, name, value, mode)`;
- `GetXAttr(ino, name, value_out)`;
- `ListXAttrs(ino, names_out)`;
- `RemoveXAttr(ino, name)`.

The metadata layer owns inode existence, create/replace preconditions,
atomicity, persistence, and `ctime` mutation. It does not understand Linux
`XATTR_CREATE` / `XATTR_REPLACE` bit values and does not construct packed FUSE
reply buffers.

### VFS

`VfsImpl` owns Linux-facing policy that should be decided once before the
backend boundary:

- only `user.*` is enabled by this version;
- Linux set flags are validated and converted to the metadata set-mode enum;
- values and name lists are returned completely to the FUSE hook.

No caller buffer size is passed into metadata. This prevents storage APIs from
being coupled to the low-level FUSE two-phase buffer protocol.

### Low-level FUSE hook

The hook owns reply sizing and wire encoding:

- for `getxattr` / `listxattr` with `size == 0`, reply with
  `fuse_reply_xattr(required_size)`;
- when a non-zero caller buffer is too small, reply `ERANGE`;
- otherwise reply the bytes with `fuse_reply_buf`;
- `listxattr` packs the complete ordered name vector as NUL-terminated names.

An empty xattr value or an empty list still follows the same two-phase rules;
the required size is zero.

## Atomicity and concurrency

Each xattr mutation is one metadata transaction. The create-only and
replace-only check observes the same inode snapshot that is rewritten, and the
`ctime` update commits with that rewrite. Concurrent writers therefore
serialize through the owning inode record rather than performing a separate
read/check/write sequence outside the transaction.

Redis uses the inode key as the optimistic-conflict boundary. Two xattr
changes to the same inode may retry even when they address different names;
this is accepted for the initial embedded design because it preserves a single
authority and simple correctness model. If profiling later demonstrates that
inode amplification or conflict granularity is material, a different layout
requires a new architecture decision rather than silently introducing a
parallel authority.

## Error model

SwordFS distinguishes unsupported callbacks from supported callbacks whose
requested operation is not available:

| Condition | errno |
| --- | --- |
| FUSE operation has no implementation | `ENOSYS` |
| xattr namespace unsupported by this implementation | `EOPNOTSUPP` |
| inode missing | `ENOENT` |
| xattr missing | `ENODATA` |
| create-only name already exists | `EEXIST` |
| invalid set flags / malformed request | `EINVAL` |
| get/list destination buffer too small | `ERANGE` |
| xattr name/value or packed-name-list limit exceeded | `ERANGE` |

Internal `Status` codes represent the first five metadata/VFS outcomes where
needed. `ERANGE` for the get/list caller-buffer protocol is produced directly
at the FUSE hook, because buffer sizing is not a metadata failure.

## Resource bounds

SwordFS follows the Linux VFS ceilings for one xattr: the fully-qualified name
is at most 255 bytes and one value is at most 64 KiB. The VFS checks these
limits before handing a request to metadata, and the inode semantic API repeats
them so non-FUSE metadata callers cannot construct canonical state that the
filesystem surface would reject.

The embedded representation also needs an intentional aggregate policy.
SwordFS keeps the packed xattr-name list at or below 64 KiB so every stored set
remains retrievable through Linux `listxattr`. It does **not** impose an
additional aggregate value-bytes or attribute-count limit in v1. Authoritative
fstests `generic/020` requires a 64 KiB value to remain legal while another
attribute already exists, so a 64 KiB aggregate value cap is too restrictive;
there is no Linux-wide aggregate value ceiling to substitute here, and the peer
JuiceFS VFS layer likewise does not define one. The embedded representation's
whole-inode serialization and rewrite amplification remain an accepted v1
trade-off. Any future aggregate cap must be justified by measured workload or
backend pressure rather than an arbitrary constant, and must preserve the
established conformance contract.

## JuiceFS peer review

JuiceFS provides the closest mature peer implementation. Its xattr path was
reviewed across `pkg/vfs`, `pkg/fuse`, and the Redis/KV metadata backends.

### Adopt / absorb

- Keep namespace/name/flag policy above the storage backend.
- Give metadata semantic set/get/list/remove operations rather than exposing
  FUSE buffer conventions.
- Preserve create-only / replace-only checks atomically with mutation.
- Return complete xattr data to the FUSE adapter, which then implements the
  kernel's size-probe and `ERANGE` behavior.
- Test missing attributes, binary/value replacement, create/replace modes,
  list encoding, and small-buffer behavior as user-visible contracts.

### Adapt

JuiceFS stores raw xattrs separately from its inode representation (for
example, a Redis Hash per inode and key ranges in KV backends). SwordFS keeps
the same semantic boundary but embeds the mapping in `SwordFsInode`. This
fits SwordFS' current compact metadata model and makes hard-link/reclaim
lifecycle correctness structural instead of requiring coordinated deletion of
a second key family.

### Do not adopt

- Do not copy JuiceFS ACL interpretation into this raw foundation. ACL is a
  separate semantic layer with different inheritance/authorization concerns.
- Do not introduce backend-specific xattr keys merely because JuiceFS uses
  them; SwordFS has not demonstrated the scale/conflict need that would repay
  the extra lifecycle authority.
- Do not add compatibility machinery for the superseded beta inode encoding.

### SwordFS differentiation

The initial design intentionally chooses one persisted inode authority. It accepts
larger inode rewrites and same-inode Redis transaction conflicts in exchange
for simpler lifecycle invariants and fewer independent persistent records.

## Verification contract

This is a C++ production feature and follows test-first development.

Before production implementation, focused tests establish RED for:

1. inode codec round-trip with binary xattr values and deterministic encoding;
2. Memory and Redis semantic parity for upsert/create-only/replace-only,
   missing inode/attribute results, ordered listing, remove, and `ctime`;
3. inode-identity lifecycle across hard link, rename, unlink, and reclaim;
4. VFS flag and namespace translation;
5. low-level FUSE get/list size probes, successful buffer replies, `ERANGE`,
   empty results, `ENODATA`, and `EOPNOTSUPP`.

After the focused RED -> GREEN loop, the Draft PR runs the repository's full
required CI matrix. Every changed non-test C/C++ production file must have
Codecov patch coverage above 90% unless an explicit, evidence-backed per-file
exception is documented. The affected fstests `user.*` population is then
reclassified from authoritative CI evidence; ACL-gated and other unsupported
namespace cases are not promoted merely because raw xattr storage exists.

The first authoritative full fstests run with the xattr implementation
(`36409089875`) removed the `user.*` prerequisite for all 19 targeted rows. Of
those, 12 ran to PASS and are admitted to `supported.txt`; the remaining seven
advanced to deeper prerequisites and stay classified by that observed reason
(`trusted.*`, fallocate/fiemap, rename whiteout, or FUSE filename semantics).
This distinction is intentional: xattr support exposes deeper requirements but
does not claim support for them.

## Performance considerations

Set/remove rewrites the complete serialized inode record and Redis conflicts
are inode-key granular. Reads deserialize the inode record just as other inode
lookups do. The first implementation adds no additional network round trip or
second persistent key lookup for xattrs. These trade-offs are deliberate and
should be revisited only with measurement showing that xattr-heavy workloads
make the embedded representation material.
