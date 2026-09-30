# Immutable and append-only inode policy

Issue #391 adds the smallest coherent SwordFS policy for Linux immutable and
append-only inode flags.  The flags are filesystem semantics, not generic
`ioctl` pass-through state: they are durable inode attributes, enforced by the
normal metadata/data mutation paths, and merely controlled through a narrow
Linux ioctl surface when that surface is enabled for a mount.

## Authority and representation

`SwordFsAttr` is the single authority for two SwordFS-owned typed bits:

- immutable;
- append-only.

Linux UAPI values (`FS_IMMUTABLE_FL`, `FS_APPEND_FL`, `FS_XFLAG_IMMUTABLE`,
and `FS_XFLAG_APPEND`) are never persisted.  The inode codec serializes the
SwordFS representation together with the other inode attributes, so Memory and
Redis observe the same durable state.  SwordFS is beta; this layout change has
no compatibility or migration path for older metadata.

Flag changes use a dedicated metadata operation rather than overloading
ordinary `setattr`.  A successful effective change updates ctime.  Reapplying
the same value is idempotent and does not manufacture a ctime change.

## Policy helpers and transaction boundary

Policy predicates are defined once over `SwordFsAttr` and reused by both
metadata backends.  Namespace and durable-metadata mutations evaluate them
after loading every authoritative inode involved in the existing transaction.
No VFS-only precheck is treated as correctness authority.

The required policy is:

| Mutation | Immutable inode | Append-only inode |
| --- | --- | --- |
| writable open / ordinary write | reject | require `O_APPEND`; reject `O_TRUNC` |
| truncate / explicit size change | reject | reject |
| chmod / chown | reject | reject |
| explicit atime/mtime | reject | reject |
| touch-now (`UTIME_NOW`) | reject | allow |
| xattr mutation | reject | reject |
| hard-link source | reject | reject |
| unlink / rmdir / rename inode | reject | reject |

For a directory carrying policy:

- immutable rejects adding, removing, replacing, or exchanging entries;
- append-only allows pure additions but rejects removal, replacement, exchange,
  or moving an existing entry out of the directory;
- neither flag is inherited by newly created children.

Implicit read-driven atime updates remain allowed.  Consequential mtime/ctime
updates and killpriv processing caused by an otherwise permitted append write
also remain allowed.

Linux FUSE represents `utime(path, NULL)` / `UTIME_NOW` by carrying the base
timestamp bit together with the corresponding `*_NOW` bit.  Policy therefore
distinguishes an explicit timestamp (`ATIME`/`MTIME` without `*_NOW`) from a
touch-now request (`ATIME|ATIME_NOW` / `MTIME|MTIME_NOW`); append-only permits
the latter while immutable rejects both.

## Open handles and data-path ordering

Opening an existing file returns its authoritative size and policy bits from
the same metadata read.  `FileReadWriter` keeps that policy snapshot beside the
existing mount-local live-size state.  Each `FileHandle` retains its original
open flags and supplies them to writes so append-only can distinguish an
`O_APPEND` handle from another writable handle.

An Open does not hold the inode operation lock across the metadata round trip
or while reconciling the returned policy, because a last-close Flush may hold a
shared operation reference while waiting for storage.  Instead a small
policy-only mutex protects a mount-local policy epoch and Open reconciliation,
while the policy bits themselves are atomically readable by the write hot path.
Every successful same-mount flag mutation, still serialized by the exclusive
inode operation boundary, publishes the new bits and advances that epoch.  If
the epoch changed while Open was in flight, the returned metadata policy is
stale with respect to the completed local mutation and must not overwrite the
newer local policy; Open is authorized against that newer policy instead.  This
preserves same-mount revocation and concurrent-open/close progress without
adding a metadata round trip or an extra mutex acquisition to writes.

CREATE initializes only the newly created inode's local authoritative size.
The `FileReadWriter` default policy is already `kNone`, so CREATE must not write
`kNone` into shared live policy state: a concurrent local flag transition may
have committed after the create operation obtained/published the shared inode
handle, and initialization cannot roll that newer policy back.

Writes hold the existing shared per-inode operation lock.  A local flag change
uses the exclusive operation boundary.  When enabling immutable or append-only,
accepted dirty data is flushed before the metadata flag commit while later
writes remain excluded.  After the commit the local policy snapshot is updated
before releasing the boundary.  Thus writes admitted before the transition
linearize before it, while later writes observe the new policy immediately.

This intentionally does not add a metadata lookup to every write.  Revocation
of a pre-existing handle on another mount requires distributed inode/session
coherence and remains owned by #402.

## Linux ioctl control surface

The mount CLI option `--enable-ioctl` defaults to false.  Its resolved value is
captured in the process-global, mount-local `MountRuntimeBehavior` singleton;
VFS hot paths do not read `ConfigCenter` directly.

When disabled, inode policy is still enforced, but the mutable ioctl control
surface is unavailable.  When enabled, only these commands are accepted:

- `FS_IOC_GETFLAGS` / `FS_IOC_SETFLAGS`;
- `FS_IOC_FSGETXATTR` / `FS_IOC_FSSETXATTR`.

For the legacy GETFLAGS/SETFLAGS pair, SwordFS follows the Linux FUSE
`fileattr_get` / `fileattr_set` transport rather than `_IOC_SIZE(cmd)`: the
kernel private FUSE ioctl carries the flag word as `unsigned int` (32 bits)
even though the public ioctl command encodes `long` on 64-bit systems.  The
daemon therefore consumes and produces exactly that 32-bit wire value.  This
is required for ordinary `chattr` / `lsattr`, whose VFS path is translated
through the kernel fileattr layer before the FUSE request reaches userspace.

GET exposes only immutable/append-only bits.  `FSGETXATTR` zeroes all other
`fsxattr` fields.  SET rejects unsupported requested bits or non-zero unsupported
fields rather than silently accepting state SwordFS does not persist.  Unknown
commands return `ENOTTY`; enabling the option never permits generic forwarding.

Linux VFS owns fileattr authorization, including inode ownership and
`CAP_LINUX_IMMUTABLE`.  SwordFS does not inspect `/proc`, reconstruct
capabilities, or equate uid 0 with that capability.  The daemon validates the
supported bit contract and persists the already-authorized request.

The low-level FUSE callback treats `arg` only as the ioctl address token.  It
uses restricted-ioctl input/output buffers supplied according to `cmd`, replies
with `fuse_reply_ioctl()` when complete, and uses `fuse_reply_ioctl_retry()`
only when one of the selected commands arrives without its required restricted
buffer.  Arbitrary unrestricted retry is not supported.

At FUSE initialization, an opt-in mount requests `FUSE_CAP_IOCTL_DIR` when the
kernel advertises it so the same selected commands work on directories.  An
opt-out mount explicitly leaves that directory-ioctl capability disabled.

## statx projection

`SwordFsAttr::ToStatX()` advertises
`STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND` in `stx_attributes_mask` and sets
the corresponding `stx_attributes` bits from the same authoritative inode
flags.  No separate statx state is introduced.

The pinned Linux FUSE kernel path cannot currently expose those two attribute
bits to applications.  `fuse_do_statx()` restricts the returned VFS result to
basic stats plus birth time and does not copy the FUSE protocol's
`attributes`/`attributes_mask` fields into `kstat`; FUSE fileattr SET also
forwards the private ioctl without updating the kernel inode's
`S_IMMUTABLE`/`S_APPEND` flags.  SwordFS keeps the internal projection correct
for a future kernel transport that consumes these fields, while fstests
`generic/424` is classified as not applicable to the pinned FUSE environment
rather than pretending the daemon can bypass the kernel boundary.

## Reference-system decisions

JuiceFS provides the closest peer implementation.  SwordFS adopts inode-centric
persistent policy, a compact internal flag representation, a narrow ioctl
surface, an explicit capability gate, and kernel-owned authorization.  SwordFS
adapts those principles to its own `SwordFsAttr`, Memory/Redis transaction
boundaries, local buffered-write model, and statx projection.  It does not copy
JuiceFS internal numeric flag layout or broaden ioctl support beyond the four
selected commands.  The explicit same-mount write/flag ordering above is a
SwordFS-specific consequence of its shared `FileReadWriter` buffering model.

## Verification invariants

Tests cover typed flag persistence and statx projection, Memory/Redis mutation
parity, mount gating, ioctl translation/buffer behavior, directory and inode
mutation policy, open/write rules including existing local handles, ctime on
effective flag changes, and the conformance mount wiring.  Formal fstests then
re-evaluates `generic/079`, `424`, `545`, and `555`; `generic/553` is reconciled
with #382 because its final copy-range behavior belongs there.
