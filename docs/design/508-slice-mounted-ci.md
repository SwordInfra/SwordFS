# #508 — Opt-in mounted Slice CI before physical reclamation

## Contract and scope

The Slice record model, immutable Put, ordered read/Flush and persistent truncate
exist independently of the actual Volume format/mount path. The mounted COW CI
cannot establish these contracts for Slice. This task creates a distinct
**experimental opt-in Slice CI volume**, without changing `cow` defaults or
claiming general Slice production readiness (#275).

## Invariants

- A volume has exactly one persisted chunk mechanism. `swordfs format
  --chunk-type chunk_slice` explicitly opts into the incomplete Slice
  lifecycle; default `cow` and unsupported `redis_cache` retain their behavior.
  Keep the original COW format-time metadata capability validation intact.
  Slice Format creates persisted volume/allocator state only; the Slice
  metadata capability is constructed on mount **after** the persisted
  ChunkSize is loaded, because pre-format Slice metadata initialization would
  reject an uninitialized Redis ChunkSize.
- Slice FileMetadata remains the sole authority for inode EOF and attachment;
  the mechanism owns its private ordered records. Normal sequential read,
  write, Flush, fsync, sparse hole, truncate and remount go through the real
  FUSE/Redis/MinIO path. Two-mount cross-domain races keep owner-approved
  #424 LWW semantics and documented beta limitations.
- Until #273, **no Slice private index or physical object may be deleted**.
  Logical detach and final inode reclaim still occur and generate distinct,
  durable opaque maintenance identities; records stay pending for #273, not
  spuriously acknowledged, and must not enter the COW destructor.
- The Slice GC worker is not started prior to #273; absent physical cleanup
  is explicit and does not slow foreground operations with repeated re-scans.
  Test workloads may leak physical Slice objects by design. This is **not**
  production readiness and must not become the default format.
- **Preserve the entire original COW Release E2E job unchanged**, including
  its status name and command. Instead, reuse the existing Debug build-and-test
  runner's already-built FUSE E2E binary and already-provisioned native tools
  for a new full opt-in Slice E2E step after coverage gating. This is the
  same runner which already ran real Redis/MinIO focused coverage scenarios.
  Each E2E invocation starts and tears down fresh Redis/MinIO and formats an
  independent volume. This avoids adding another cold APT install/build and
  keeps baseline COW release regression validation separate from Slice tests.

## Verification plan

1. TDD RED: real Redis Volume format/mount with explicit `chunk_slice` must
   fail before the integration gates are added. Focused Dev Build verifies it.
2. GREEN: enable opt-in Slice with deferred cleanup identities; test real Redis
   backend volume format/mount, plus mounted FUSE CRUD, Flush, remount and
   truncate using pinned MinIO and Redis in CI.
3. Validate opt-in Slice full mounted E2E from the existing Debug CI runner,
   while leaving the required COW Release E2E job untouched. Record actual
   POSIX test status; pjdfstest/fstests remain COW-only until extended under
   #275. Keep #273, #274, #202, #275 outstanding.

## Peer reference

JuiceFS's per-file SliceRecord overwrite/zero-cover semantics are useful for
our read and truncate expectations. SwordFS must instead retain its
mechanism-independent ChunkID attachment and private Redis Slice index, and
cannot borrow a generic shared cleanup path from COW or assume global
metadata/physical delete coordination.
