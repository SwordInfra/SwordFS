# POSIX authorization

## Purpose

SwordFS uses Linux FUSE as its POSIX entry point. Ordinary POSIX DAC must have
one owner. SwordFS must not maintain a second, partial implementation of Linux
mode-bit authorization that can disagree with the kernel about path search,
supplementary groups, capabilities, descriptor access, or setattr semantics.

The production rule is:

> Linux VFS/FUSE owns ordinary POSIX DAC. SwordFS owns per-open descriptor
> capabilities plus filesystem state and structural invariants after the
> kernel has authorized the operation.

## Authorization boundary

SwordFS mounts FUSE with `default_permissions`. For normal FUSE requests the
kernel therefore evaluates mode/uid/gid access before dispatching the low-level
operation to SwordFS. This includes pathname search permission and the generic
setattr/open authorization rules implemented by Linux VFS.

```text
calling process credentials
        |
        v
Linux VFS / FUSE default_permissions
  - pathname search (directory X_OK)
  - owner/group/other mode bits
  - supplementary groups
  - Linux capability overrides
  - chmod/chown/truncate/time setattr authorization
  - descriptor-aware ftruncate authorization
        |
        | authorized request only
        v
SwordFS FUSE callback
        |
        +--> FileHandle (per-open state)
        |      - readable / writable capability
        |      - append/open-description state when needed
        |
        v
VFS / metadata
  - sticky-directory ownership safety
  - type and namespace invariants
  - metadata transaction atomicity
  - inode/chunk lifetime
  - data publication and reclaim
  - required mutation side effects
```

`allow_other` does not replace permission checking. It only permits processes
other than the mount owner to reach the filesystem; `default_permissions`
provides the normal kernel DAC gate for those processes.

## Why userspace DAC is not authoritative

The FUSE request context currently exposes the request uid/gid/pid/umask, but
SwordFS does not own the complete Linux credential model. Correct DAC may depend
on supplementary groups and capabilities, and descriptor-based operations carry
state that the kernel already has authoritatively.

Reconstructing those semantics independently in every metadata operation would
create two authorization engines:

```text
kernel decision != SwordFS userspace decision
```

That can produce both security bugs (an operation is accepted when it should be
denied) and false denial (the kernel correctly accepts a supplementary-group or
capability case that SwordFS then rejects).

Accordingly, metadata code must not use incomplete mode-bit checks as a second
FUSE authorization gate. Metadata may still validate structural conditions
that are part of filesystem correctness rather than caller DAC. In particular,
sticky-directory ownership protection remains a cheap transaction-local safety
invariant: it is redundant with the normal kernel path, but protects namespace
mutation semantics if an internal caller reaches metadata after bypassing that
path.

## Open and create

For an existing inode, Linux evaluates the requested access mode before FUSE
OPEN when `default_permissions` is active. SwordFS Open therefore does not
re-run a hard-coded `R_OK` check. It validates/open-tracks the inode, records
the access capability implied by `fi->flags` in the per-open `FileHandle`, and
performs only SwordFS-specific state work.

CREATE is one compound operation from the caller's perspective. After the
kernel has authorized creation in the parent and SwordFS successfully creates
the inode, SwordFS must establish the returned file handle directly rather than
re-entering the existing-inode Open path. The newly created inode's final mode
does not retroactively reject that already-authorized open. In particular,
`open(O_CREAT|O_RDWR, 000)` may return a usable descriptor.

### Create-time ownership and SGID inheritance

Authorization and ownership assignment are separate responsibilities. Linux
VFS/FUSE `default_permissions` decides whether the caller may create the entry;
after that decision, SwordFS persists the Linux create-time ownership side
effects from the request identity and the authoritative parent inode.

For all inode-creating namespace operations, Memory and Redis use the same
rule:

```text
ordinary parent directory:
  child.uid = caller uid
  child.gid = caller gid

SGID parent directory:
  child.uid = caller uid
  child.gid = parent gid
  if child is a directory:
    child.mode |= S_ISGID
```

The parent mode/gid used for this decision must come from the same metadata
transaction that publishes the child, so a concurrent parent ownership/mode
change cannot be combined with a stale inheritance decision. This rule also
applies to non-directory creation paths such as mknod and symlink for group
ownership; directory SGID propagation applies only to child directories.

SwordFS does not add a second supplementary-group or capability authorization
engine to decide this rule. In particular, handling of an explicitly requested
SGID bit on a non-directory is separate from the parent-directory inheritance
rule and remains subject to the kernel/FUSE authorization boundary.

## Setattr and truncate

Linux FUSE calls the VFS setattr preparation path before emitting FUSE_SETATTR
when `default_permissions` is enabled. The kernel therefore owns generic DAC
for pathname truncate, chmod/chown, and timestamp updates.

For descriptor-based ftruncate, FUSE also supplies the open handle. SwordFS
must preserve that `fh` through the callback and validate the descriptor's own
writable capability before applying a size mutation. This is not a second DAC
engine: it is an invariant of the open file description SwordFS itself owns.
Pathname-based truncate has no `fh` and relies on the kernel authorization that
already occurred.

SwordFS otherwise consumes the authorized mutation and applies
persistent/data-state changes. It should not add a second uid/gid permission
decision solely because FUSE SETATTR contains an inode number rather than a
pathname.

### setid / killpriv boundary

Linux VFS owns the decision whether a metadata or data mutation must clear
set-user-ID/set-group-ID privilege. SwordFS must not reconstruct that decision
from uid/gid changes, size changes, request credentials, supplementary groups,
or capability guesses.

When FUSE delivers an explicit kill-suid/sgid SETATTR signal, SwordFS applies
one mode transformation to authoritative metadata:

```text
clear S_ISUID
clear S_ISGID only when S_IXGRP is set
```

This distinction matters because SGID on a non-group-executable regular file
is not the executable setgid privilege bit and must be preserved.

The current libfuse 3.18.2 public low-level API exposes the SETATTR kill bit,
but does not expose the protocol's `FUSE_OPEN_KILL_SUIDGID` or
`FUSE_WRITE_KILL_SUIDGID` bits to OPEN/CREATE/WRITE callbacks. Therefore
SwordFS must not advertise userspace killpriv handling through either
`FUSE_CAP_HANDLE_KILLPRIV` or `FUSE_CAP_HANDLE_KILLPRIV_V2`. Until the callback
API can carry all V2 signals, SwordFS uses the kernel's legacy killpriv
path:

- leave both `FUSE_CAP_HANDLE_KILLPRIV` and
  `FUSE_CAP_HANDLE_KILLPRIV_V2` disabled;
- leave `FUSE_CAP_ATOMIC_O_TRUNC` disabled so truncate privilege changes are
  expressed through kernel SETATTR/MODE handling rather than hidden inside
  OPEN;
- treat metadata `Truncate()` as size/chunk mutation only;
- treat WRITE and `CommitChunk()` as data/publication operations only, never as
  delayed privilege-decision boundaries.

The kernel's explicit MODE adjustment in the legacy path remains authoritative
and is applied like any other authorized SETATTR. A future switch to V2 is
valid only when SwordFS can consume SETATTR, OPEN/O_TRUNC, and WRITE kill
signals consistently before acknowledging the corresponding request.

Live file-size composition remains governed by `live-inode-state.md`; truncate
replaces the logical size rather than behaving as a write high-water mark.

## Sticky directories and namespace mutations

Linux VFS evaluates directory DAC and sticky-directory delete/rename policy
before invoking the FUSE namespace operation. SwordFS metadata still keeps the
sticky ownership rule as a transaction-local safety invariant, alongside
namespace structure (type compatibility, existence, cycle prevention, link
counts, orphan transitions, etc.). Ordinary mode-bit DAC is not duplicated.

The distinction is intentional: parent `W_OK|X_OK` checks are generic DAC and
belong to the kernel; sticky ownership depends only on the directory/entry
owners already present in the transaction and protects a namespace mutation
invariant without reconstructing Linux supplementary groups or capabilities.

## Non-FUSE callers

The current production POSIX contract is the FUSE mount. Internal metadata APIs
are not a second public POSIX interface and should not force the FUSE path to
carry a duplicate permission engine. If a future non-FUSE public interface needs
POSIX caller authorization, it must establish an explicit authorization boundary
for that interface instead of implicitly reusing partial FUSE checks.

## Testing

`pjdfstest` is the authoritative end-to-end oracle for the permission surface.
Regression coverage must include:

- denied directory search in a path prefix;
- owner/non-owner chmod and chown;
- truncate and `O_TRUNC` write authorization;
- descriptor-based ftruncate and create-open behavior;
- explicit vs `UTIME_NOW` timestamp authorization;
- sticky-directory rename/delete behavior;
- successful cases using supplementary groups where applicable.

Unit tests should cover SwordFS-specific state/API behavior below the FUSE
boundary, but should not reimplement Linux's DAC matrix in metadata mocks.
