# POSIX ACLs

## Purpose

SwordFS stores Linux POSIX access/default ACLs in the existing inode-owned
extended-attribute map introduced by #378. ACLs do not create a second metadata
authority: durable state is the canonical Linux xattr bytes in
`SwordFsInode::xattrs` together with the synchronized permission projection in
`SwordFsInode::attr.mode`.

Linux VFS remains the authorization authority for FUSE mounts. SwordFS does not
implement a second ACL access evaluator.

## FUSE authority contract

POSIX ACL is a persistent volume feature selected by
`swordfs format --enable-posix-acl`; it defaults to disabled. SwordFS does not
provide a per-mount ACL toggle because two mounts of the same volume must not
choose conflicting persistence, inheritance, or mode-projection semantics.
The feature bit is stored in the canonical `SwordFsVolume` record using the
current beta schema directly; no compatibility decoder is retained for the
previous beta record layout.

ACL semantics are active only when that persisted volume feature is enabled
and the kernel advertises both `FUSE_CAP_POSIX_ACL` and
`FUSE_CAP_DONT_MASK`. SwordFS requests both or neither.
`FUSE_CAP_POSIX_ACL` keeps access checks in Linux VFS together with
`default_permissions`; `FUSE_CAP_DONT_MASK` delivers the unmasked create
mode and caller umask so create-time ACL/umask derivation happens exactly once.

`FUSE_CAP_SETXATTR_EXT` remains disabled with libfuse 3.18 because the
low-level callback does not expose `FUSE_SETXATTR_ACL_KILL_SGID`.

When the volume feature is disabled, or when the capability pair is
unavailable, the exact
`system.posix_acl_access` and `system.posix_acl_default` names remain
unsupported at the FUSE/VFS boundary. Raw `user.*` xattrs continue to use
the #378 path; other `system.*` and `trusted.*` names stay unsupported.

## Durable representation

ACL xattrs use Linux xattr version 2 and the canonical packed
`posix_acl_xattr_header` / `posix_acl_xattr_entry` byte layout. A transient
typed `PosixAcl` parser owns validation and transformations:

- exact entry framing and version;
- legal tags and permission bits;
- exactly one owner, owning-group, and other entry;
- canonical tag-group ordering with unique named IDs in any order within each
  named-user/named-group run;
- undefined IDs on object/mask/other entries and concrete IDs on named entries;
- no duplicate named entries;
- a mask for every ACL containing named users/groups.

The typed object is transient. No ACL IDs, side keys, caches, or deduplication
tables become durable authorities.

An access ACL containing only owner/group/other entries is mode-equivalent and
is canonicalized away after projecting those permissions into `attr.mode`.
Extended access ACLs remain stored. Default ACLs remain stored on directories
and never alter directory mode solely by being set or removed.

## ACL and mode synchronization

Setting an access ACL and its permission-mode projection is one inode metadata
mutation:

- owner bits come from `ACL_USER_OBJ`;
- group-class bits come from `ACL_MASK` for an extended ACL, otherwise
  `ACL_GROUP_OBJ`;
- other bits come from `ACL_OTHER`.

File type and special bits are preserved except where the existing kernel
killpriv/setid contract explicitly changes them.

`chmod` on an inode with an extended access ACL updates
`ACL_USER_OBJ`, `ACL_MASK`, and `ACL_OTHER` to the requested owner/group/
other classes while preserving named entries and `ACL_GROUP_OBJ`. Mode and
rewritten ACL are committed through the same Memory/Redis inode transaction.

Removing an access ACL leaves the already synchronized mode as the authority.
Successful effective ACL/mode mutations update ctime through the existing inode
mutation path.

## Create inheritance

Create-time ACL state is derived while the authoritative parent inode is held
in the same metadata transaction that publishes the child.

Without a parent default ACL, apply `SwordFsContext::umask` once to the
unmasked requested permission bits, then apply the existing parent-SGID
gid/directory-SGID inheritance rule. Umask affects only the ordinary `0777`
permission classes; special mode bits that Linux has already authorized and
delivered in the create request (for example `S_ISGID`) must survive this
metadata projection rather than being truncated before inheritance runs.

With a parent default ACL, do not apply umask separately. Copy the default ACL
into a child access ACL, intersect its owner, group-class, and other classes
with the requested mode, project the result into child mode, and store the
access ACL only when it remains extended. A child directory also receives the
parent default ACL unchanged.

Regular files and mknod-created non-directory inodes inherit access ACLs.
Directories inherit access plus default ACLs. Symlinks inherit neither ACL and
keep the existing symlink-mode behavior.

Child mode, ACL xattrs, gid/SGID inheritance, and namespace publication cross
one Memory/Redis transaction boundary.

## Backend boundaries

The ACL parser/transformation layer is backend-neutral. Memory and Redis invoke
the same semantic helpers from their existing inode transaction primitives:

- `SetXAttr` / `RemoveXAttr` for ACL mutation;
- `SetAttr` for chmod synchronization;
- create/mkdir/mknod transaction paths for inheritance.

Redis continues to rewrite the single serialized `inode:<ino>` record.
Memory mutates the same in-memory `SwordFsInode`. Rename, hard links, unlink,
and final reclaim therefore inherit the #378 lifecycle guarantees without
ACL-specific lifecycle code.

No inode codec schema bump is required. #378 already made the ordered xattr
map part of the version-1 `SwordFsInode` serialization, and #396 stores ACLs
inside that existing field rather than adding a new record field. SwordFS is
still beta, so no compatibility decoder or dual-format ACL representation is
introduced.

## Verification

The implementation follows test-first development. Unit tests cover malformed
ACL rejection, canonicalization, mode projection, chmod synchronization,
default-ACL placement, umask/default-ACL inheritance, Memory/Redis parity, and
FUSE capability/namespace routing. Formal CI then reruns the selected ACL-gated
fstests and reconciles the conformance baseline only from fresh raw outcomes.

SGID-sensitive cases are evaluated with `FUSE_CAP_SETXATTR_EXT` deliberately
disabled. If their only remaining blocker is the missing libfuse transport for
`FUSE_SETXATTR_ACL_KILL_SGID`, that limitation is tracked separately rather
than emulating kernel capability logic in SwordFS.
