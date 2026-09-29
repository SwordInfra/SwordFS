# statx and inode birth-time authority

## Context

SwordFS exposes the normal inode attributes through `getattr`, but the low-level
`statx` operation currently returns `ENOSYS`. Linux `statx(2)` also has a
creation-time field that cannot be derived correctly from `ctime`, `mtime`,
or `atime`: all three are mutable after inode creation.

The metadata inode is the authority for filesystem attributes. `statx` must be
another projection of that authority, not a second metadata model.

## Invariants

- `SwordFsAttr::btime` and `btime_nsec` are the authoritative inode creation
  timestamp.
- Birth time is assigned once when a new `SwordFsAttr` is constructed for an
  inode and is never an input to `setattr`.
- Namespace changes, hard links, attribute changes, writes, and truncation
  preserve birth time because they mutate or copy the same inode attributes
  rather than recreating inode identity.
- Memory metadata and Redis metadata expose the same contract. Redis persistence
  comes from the normal inode codec; Memory persistence is the in-memory inode
  object itself.
- `getattr` and `statx` resolve the same live inode state before projecting
  it. This includes mount-local live size overlays and the retained detached
  inode behavior used after unlink.
- `statx` reports only fields for which SwordFS has an authority. It does not
  synthesize unsupported Linux extensions.

## Metadata representation

`SwordFsAttr` stores signed seconds and nanoseconds for birth time, matching
the existing atime/mtime/ctime representation. New inode construction captures
one creation instant and uses it for atime, mtime, ctime, and btime initially.
Subsequent timestamp mutation affects only the timestamp selected by that
operation.

The current inode codec encodes `btime` and `btime_nsec` directly in the
existing schema-v1 attribute payload. SwordFS is still beta, so this changes the
current layout in place:

- `kSchemaVersion` remains 1.
- There is no decoder fallback for the previous beta layout.
- There is no dual-read path, migration, or compatibility branch.
- A schema-v1 inode payload lacking the birth-time fields is malformed under the
  new target layout.

## stat and statx projections

`SwordFsAttr::ToPosixStat()` remains the `struct stat` projection.
`SwordFsAttr::ToStatX()` is the corresponding `struct statx` projection.

The first `statx` implementation advertises:

- `STATX_BASIC_STATS`
- `STATX_BTIME`

This includes inode type/mode, link count, uid/gid, atime, mtime, ctime, inode
number, size, block count, and birth time. Device numbers and preferred block
size are also populated from the same attributes, as required by the statx ABI,
but do not have separate request-mask bits.

`stx_attributes` and `stx_attributes_mask` remain zero. Immutable and
append-only inode-flag reporting belongs to #391.

The block-count projection intentionally shares the current `stat(2)`
semantics, including the existing non-empty regular-file fallback used while
precise sparse allocation accounting is pending. This prevents `stat` and
`statx` from becoming separate block-accounting authorities. Sparse-aware
precision remains owned by #370.

## VFS and FUSE flow

The VFS has one helper that resolves the current inode state:

1. use an existing inode handle when one tracks live local state;
2. otherwise read the metadata inode;
3. if authoritative metadata reports the inode gone, consult the retained
   detached-inode cache;
4. refresh the retained cache on success.

`GetAttr` projects that result with `ToPosixStat()`; `StatX` projects the
same result with `ToStatX()`.

The low-level FUSE callback preserves nullable `fuse_file_info *` handling.
On success it replies with `fuse_reply_statx`; on failure it translates the
SwordFS status through the existing errno path. Request flags and masks do not
create metadata authority and do not cause unsupported fields to be fabricated.

## JuiceFS peer review

JuiceFS keeps normal inode timestamps in its authoritative metadata `Attr` and
has VFS attribute reads resolve from that same metadata object. That single
attribute-authority principle is useful and is retained.

Its current Linux metadata model has no independent birth-time field. The
WinFSP projection maps atime into a birth-time slot, which is unsuitable for
SwordFS because atime is mutable.

- **Adopt:** one authoritative inode-attribute model and projection from it.
- **Adapt:** use SwordFS's existing live-inode overlay/detached-inode resolution
  before both stat projections.
- **Do not adopt:** deriving birth time from atime, ctime, or any other mutable
  timestamp.
- **SwordFS differentiation:** persist an explicit immutable birth timestamp and
  expose it through an honest Linux `STATX_BTIME` contract.

## Verification

The feature is verified test-first at three boundaries:

- metadata type/codec tests: statx projection, birth-time round trip, malformed
  previous beta layout rejection, and nanosecond validation;
- Memory and Redis metadata tests: inode creation assigns birth time and common
  identity/attribute mutations preserve it;
- VFS/FUSE tests: statx observes the same live inode state as getattr, preserves
  nullable file-info handling, replies with statx data on success, and forwards
  metadata errors.

After focused unit tests and the full PR matrix are green, `generic/528` is
run in the authoritative fstests harness. Its known-gap baseline is promoted
only after that testcase passes.
