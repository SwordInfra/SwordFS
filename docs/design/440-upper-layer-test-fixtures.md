# Upper-layer test independence from Memory Metadata (#440)

## Goal and boundaries

Retiring the production Memory metadata backend must not erase assertions
about VFS/FUSE behavior, orphan handoff, chunk cleanup and retry/failure
handling. Test doubles are allowed to implement **only exercised contracts**;
they must not become a second metadata namespace engine or transaction store.

## Test domains

| Domain | Required observable semantics | Replacement strategy |
| --- | --- | --- |
| Chunk and FileReadWriter | Typed COW allocation, revision, head CAS; failed publication | `TestCOWChunkMetadata` (test-only, no namespace implementation), existing mocked `IMetaEngine` and data engine |
| Volume runtime common fixtures | Compose COW typed metadata when a simple test `IMetaEngine` reports unsupported | Same small test-only typed COW double; retain `ConfiguredMetaEngine` adapter |
| Reclaimer error/scan/worker tests | Visitor abort, restart/retry, failed scans and deferred deletion | `UnsupportedMetaEngine` is a default-failing interface adapter; derived test doubles override only their injected visitor or error behavior |
| Full orphan, Link, rename and FUSE last-link lifecycle | Authoritative namespace, atomic unlink/reclaim, POSIX callbacks | `ReclaimerRedisTest` and `VfsLastLinkCleanupTest` use `MakeFormattedRedisMetaEngine` and a unique real Redis volume; no secondary in-process namespace store |

The FileMetadata and typed ChunkMetadata domains remain independent per
#312/#394. For COW-specific cleanup fixtures, the typed test double models only
the public `COWChunkMetadata` interface; it does not allocate filesystem inode
IDs, simulate metadata transactions, or support Redis-only failure semantics.

`UnsupportedMetaEngine` implements no successful filesystem operations beyond
backend lifecycle. Its deliberately failing methods make incidental demands
visible rather than silently satisfying them with another fake namespace.
`EmptyReclaimerMetaEngine` makes only the three empty visitor scans succeed;
the self-waking multi-pass fixture also acknowledges controlled pending items
so its second scan is not obstructed by an unrelated default-failing method.
`FailingScanMetaEngine` controls a single orphan visitor injection without
creating a file; no namespace semantics are required to verify worker error
aggregation. `StagedIntentMetaEngine`, `PendingReclaimMetaEngine`,
`RawPendingDeleteMetaEngine`, `ForcedMultiPassMetaEngine`, and
`SlowOrphanScanMetaEngine` similarly override only the observed callbacks.

Redis-backed fixtures use a **new formatted volume per test** (isolated
namespace); the real engine owns namespace and reclaim authority. Because a
Redis `GetInode` requires a non-null output, tests that only care about status
read into a disposable typed inode. Visitor order is not promised by Redis;
collection assertions sort inode IDs where the test cares only about set
membership. All FUSE cleanup and cross-actor reclaim tests remain service-
backed; standalone Chunk, FileReadWriter, error, scan and failure-injection
tests remain service-free.

## Verification

- Audit the in-scope test sources for `MemMetaImpl`, `MemCOWChunkMetadata`,
  `metadata/mem/` and direct backend instantiations.
- Run Debug and Release unit tests in GitHub CI (never compile SwordFS locally).
- Keep explicit evidence of each original Reclaimer/FUSE scenario and its
  replacement. Errors and early aborts must not be silently reduced to success.
- Do not remove Memory backend production files in this Issue (#443 owns that).
