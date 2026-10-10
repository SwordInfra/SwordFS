# Redis-only metadata cutover (#443)

## Objective and authority

Remove the beta Memory metadata backend after #439's Memory test inventory,
#442's metadata contract matrix, and #440/#441's upper-layer fixture migration.
The only production metadata backend is Redis, using the existing
`IMetaEngine`, `MetaEngineRegistry`, Redis metadata record, and typed chunk
interfaces. This is an intentional beta compatibility break; **no migration,
legacy reader, file-based configuration fallback, or Memory stand-in** is added.

The #439 inventory contains 179 Memory tests: A=1, B=109, C=69.
The 69 C dispositions are authoritative in
`docs/design/442-metadata-backend-contract.md`. The 109 B cases must be
audited at the original assertion/edge-case level against real Redis witnesses
before deleting their Memory sources. A second existing test name is not
sufficient proof of parity. The detailed B audit lives in
`docs/design/443-memory-b-assertion-review.md`.

Reviewing the B assertions revealed that a few original mappings relied on
multiple Redis tests or did not prove all consequences in one operation. In
particular, #443 strengthens B005 (successful private publication after a
failed attempt), B030 (rename-overwrite victim through completed reclaim),
and B067 (POSIX default ACL across directories, regular files and symlinks).
The B004 cached-pointer equality is private to Memory's object reuse and is
not a backend-neutral identity promise. B017/B100/B112/B167 are satisfied by
composite Redis assertions detailed in the B audit. These are explicit
semantic dispositions rather than a test-count parity shortcut.

## Construction and persistent authority

### Before

```text
VolumeImpl::CreateFrom / LoadFrom
    -> MetaEngineRegistry
       -> MemMetaImpl -> VolumeFile -> /etc/swordfs/<volume>/volume.fmt
       -> RedisMetaImpl -> RedisMetaOps -> Redis volume format key
```

### After

```text
VolumeImpl::CreateFrom / LoadFrom
    -> MetaEngineRegistry
       -> RedisMetaImpl -> RedisMetaOps -> Redis volume format key
```

The metadata store is the sole authority for persisted volume identity,
configuration, namespace, inode and chunk state. The old
`memory://local` URL must fail at CLI validation, registry instantiation and
direct `VolumeImpl` creation/mount, with `ENOSYS` and no new volume state.
Redis URL parsing, schema, transactions, namespace and chunk semantics remain
unchanged. Test-only `swordfs-test-meta` is retained for offline upper-layer
error/adapter checks without persistence claims.

## Deletion and tooling changes

- Delete `src/metadata/mem` and the eight Memory-specific test sources
  **only after** the B assertion reconciliation is complete and missing
  Redis test obligations have been implemented.
- Drop production Memory registration and `kMemoryMetaUrl`; delete the
  `ValidateMetaUrl` Memory branch; retain Redis syntax validation and
  backend-neutral registry/interfaces.
- Remove five obsolete CI/Dev Build `/etc/swordfs` setup sites (each has
  `sudo mkdir -p` and `sudo chown`) in the Unit/E2E/pjdfstest/fstests
  jobs and Dev Build's optional Unit setup. Keep FUSE device/group permissions,
  `/etc/fuse.conf` provisioning, and any unrelated harness ownership fixes.
- Delete the E2E `RemoveVolumeConfig` method, its calls, and the
  CommandCoverage teardown's local configuration removal. Retain unmount,
  diagnostic, and temporary workdir cleanup.
- Update CLI help, active architecture/metadata documentation, and
  comments in `Format.cpp` and `VolumeImpl.hpp` to describe Redis-backed
  volume persistence; keep historical test audit references explicitly
  labelled as history.
- Do not change the Redis persistent schema/protocol or add compatibility
  readers for old `volume.fmt`.

## Change-size justification

The removal necessarily deletes twelve C++ source/header files comprising the
obsolete Memory implementation and eight Memory-only test sources (7,000+
lines of deleted beta code). This is deliberate subsystem retirement, not a
large new implementation or mechanical churn: the old classes/types have no
production consumers after the registry and test-fixture migration. New code
is limited to Redis semantic regression tests and unsupported-scheme checks.
No second in-memory namespace, file persistence shim, or test-only production
API is added to replace what was deleted.

## Verification and merge gates

1. All 109 B rows have an explicit assertion-level disposition and all
   identified material missing Redis semantic boundaries are tested prior to
   deletion. Preserve the C disposition document.
2. A focused GitHub Dev Build proves Redis backend contract/volume tests,
   then complete Debug and Release unit tests (with no skipped tests).
3. Latest formal PR checks prove Redis/MinIO E2E, pjdfstest, all applicable
   fstests shards/aggregate, static audits, Codecov per-changed-production-file
   patch coverage above 90% or an explicit exception, and formatting.
4. `git grep` verifies that no operational `/etc/swordfs`, `volume.fmt`,
   or `memory://local` path remains other than explicit negative tests,
   references to removed beta behavior in historical documents, and no live
   code that registers or uses a production Memory metadata backend.
5. Post-merge main CI and parent #438 completion are evaluated separately.
