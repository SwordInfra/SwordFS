# ioctl and block-mapping fstests applicability

Issue #383 audits the fstests cases that were originally grouped behind
`VfsImpl::IoCtl()` or FIEMAP prerequisites. The classification boundary is the
filesystem semantic being tested, not the Linux ioctl transport used by a
userspace tool such as `chattr`, `fstrim`, or `xfs_io`.

## Context

The pinned fstests revision is `a370dcbed43563f0462801e889e0eceb93c7cfad`.
At the start of the audit, 18 selected tests stopped at an ioctl-related
prerequisite and four stopped at FIEMAP. After #378 added persistent `user.*`
xattrs, `generic/425` advanced past its xattr prerequisite and exposed a fifth
FIEMAP blocker. The audit therefore covers 23 current baseline rows.

SwordFS' data path is not a local block filesystem. File offsets map to fixed
logical chunk indexes, while the selected chunk type implementation owns its private
physical representation. The current COW mechanism stores immutable
revisioned objects in S3-compatible storage; there is no filesystem block
device, discard address space, DAX mapping, allocation-group topology, or
stable physical extent address that the VFS layer can truthfully expose.

This distinction is also visible in peer behavior. At JuiceFS commit
`7ca76c6efc3de384ad0c4fbc057e2ebcd8736518`,
`pkg/vfs/vfs_unix.go` implements a small semantic ioctl set for inode flags
(`FS_IOC_GETFLAGS` / `SETFLAGS` and `FS_IOC_FSGETXATTR`) and rejects other
commands instead of providing generic ioctl pass-through. Its published POSIX
compatibility procedure also removes the LTP `ioctl_fiemap01` case from the
tests that run on JuiceFS. SwordFS should adopt that semantic separation, but
#391 still needs `FS_IOC_FSSETXATTR` because pinned `generic/555` deliberately
exercises that interface. The peer evidence supports separating meaningful
inode policy from local physical mapping rather than treating "ioctl support"
as one feature.

## Classification rules

1. An ioctl is only a transport. A case remains a SwordFS capability gap when
   the underlying semantic is meaningful for a distributed object-backed
   filesystem.
2. A current prerequisite may mask the real test contract. If implementing the
   prerequisite would only advance the case to a contract that is inherently
   local/block-filesystem-specific, classify the testcase by that final
   contract instead of promising the prerequisite solely for the test.
3. `upstream_not_applicable` is reserved for tests whose asserted behavior
   depends on concepts SwordFS intentionally does not expose, such as physical
   block discard, DAX, forced local-filesystem shutdown/journal recovery, or
   physical/shared extent topology.
4. A meaningful but unselected semantic remains `known_unsupported`. Selected
   product semantics receive a focused implementation Issue.
5. Reclassification does not make the current raw NOTRUN disappear. The
   baseline keeps the exact observed prerequisite message until another
   implementation legitimately advances the test.

## Capability dispositions

| Tests | Capability actually tested | Disposition | SwordFS boundary |
| --- | --- | --- | --- |
| `generic/079`, `424`, `545`, `553`, `555` | immutable / append-only inode policy, authorization, and reporting | **Selected capability: #391** | Persist inode policy and expose only the required Linux flag ioctls. Enforcement belongs in normal VFS/metadata mutation paths, not in a generic passthrough. `generic/553` is also dependent on #382 once immutable policy is available. |
| `generic/277` | per-inode no-atime (`FS_NOATIME_FL`) and ctime persistence | **Meaningful, deliberately unsupported** | SwordFS already owns atime/ctime state, so the semantic is meaningful. It is not part of the focused immutable/append implementation. |
| `generic/492` | mutable online filesystem label | **Meaningful, deliberately unsupported** | `SwordFsVolume::name` is a storage/metadata namespace identity, not a mutable display label. A label would need independent persisted semantics; do not mutate volume identity to satisfy this ioctl. |
| `generic/064`, `094`, `225` | FIEMAP extent enumeration/count/topology | **upstream_not_applicable** | SwordFS exposes logical chunks, not stable local physical extents. `generic/064` additionally tests fallocate insert/collapse behavior, but makes physical extent-count merging part of the expected contract; fallocate remains tracked independently. |
| `generic/425` | FIEMAP of external xattr blocks | **upstream_not_applicable** | #378 deliberately embeds xattrs in the authoritative inode record, so there are no external xattr blocks to enumerate. |
| `generic/578` | reflink plus shared/non-shared physical extent reporting | **upstream_not_applicable** | The pinned FUSE population already treats reflink as unavailable, and SwordFS has no physical sharing topology to expose through `filefrag`/FIEMAP. |
| `generic/159`, `160` | reflink/dedupe restrictions on immutable files | **upstream_not_applicable** | `chattr` is merely the first current prerequisite. The next required contract is reflink/dedupe, which the pinned FUSE model already classifies as unavailable/non-applicable. |
| `generic/260`, `288` | FITRIM/discard range argument semantics | **upstream_not_applicable** | Object storage provides no mounted block-device discard address space. |
| `generic/537` | FSTRIM against read-only / norecovery journal states | **upstream_not_applicable** | Both discard and local metadata-journal recovery modes are local block-filesystem concepts. |
| `generic/367` | physical extent-size allocation hints and allocated/delayed extent state | **upstream_not_applicable** | SwordFS allocation is governed by logical chunking and chunk-type-private object representation, not local extent-placement hints. |
| `generic/507` | inode-flag recovery across forced filesystem shutdown | **upstream_not_applicable** | The flag prerequisite is incidental; the asserted durability mechanism is forced local-filesystem shutdown/journal recovery, which pinned fstests does not support for FUSE. Inode-flag persistence itself belongs to #391. |
| `generic/508` | birth-time recovery across forced filesystem shutdown | **upstream_not_applicable** | `lsattr`/birth-time prerequisites mask a forced-shutdown/journal-recovery contract. Birth-time support itself remains owned by #381. |
| `generic/596` | XFS `xfsaild` `PF_MEMALLOC` regression during unmount | **upstream_not_applicable** | The synchronous inode flag is only test setup; the asserted bug is specific to the XFS kernel implementation. |
| `generic/607` | DAX inode-flag inheritance under DAX mount modes | **upstream_not_applicable** | DAX is a direct-access persistent-memory/block-filesystem facility and has no SwordFS object-backed analogue. |
| `generic/623` | mapped-write fault behavior after forced filesystem shutdown | **upstream_not_applicable** | The regression is about XFS shutdown/iomap behavior after a local filesystem is forced down. |

## Selected inode-flag capability

#391 owns immutable and append-only semantics because these are useful inode
policies independent of local block layout. The intended surface is deliberately
narrow: durable inode state plus the Linux flag ioctls needed by generic tools,
correct `CAP_LINUX_IMMUTABLE` behavior, mutation enforcement, ctime updates,
and statx reflection. It does not turn `VfsImpl::IoCtl()` into a generic
passthrough.

`generic/553` illustrates why capability attribution is dynamic. Today its
first blocker is immutable flag setup, so #391 owns the current known gap. Once
that prerequisite works, the case must be re-evaluated and may move to #382 if
`copy_file_range` becomes the next blocker.

## Consequences for the FUSE/VFS surface

There is no requirement to implement a generic ioctl forwarding framework.
Future selected commands should be decoded explicitly at the VFS boundary and
mapped onto typed SwordFS semantics. The low-level FUSE hook must implement the
buffer/retry/reply protocol required by those commands when #391 is developed.

No FIEMAP/BMAP compatibility layer should invent physical block addresses for
S3 objects or logical chunks. If SwordFS later needs a user-visible logical
allocation-map API for diagnostics or optimization, it should be designed from
SwordFS' own chunk/object authority rather than presented as a fake local block
map.

## Baseline result

On the post-#378 baseline, the audit moves 16 rows from `known_unsupported` to
`upstream_not_applicable`, leaves two meaningful-but-unselected semantics under
#255, and assigns five immutable/append rows to #391. Supported behavior is not
removed or promoted by this audit.

The resulting selected population remains 645 tests with 125 supported tests.
The known-gap inventory becomes 112 `known_unsupported`, 399
`upstream_not_applicable`, 8 `environment`, and 1 `upstream_test_defect`.

Any future implementation that advances one of these current prerequisite
messages must re-run the affected testcase and re-attribute it from new raw
evidence rather than preserving this audit mechanically.
