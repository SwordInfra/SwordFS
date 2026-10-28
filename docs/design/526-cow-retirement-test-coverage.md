# #526 — COW retirement test-coverage migration

Status: Stage A preparation for #521 / #527. COW remains fully functional at this
stage. Previous #496 already separated direct Slice Chunk fixtures.

## Coverage ownership inventory

| Existing coverage | Ownership | Retirement rule |
| --- | --- | --- |
| tests/unittests/chunk/cow/{COWChunkTest,COWChunkBoundaryTest,COWObjectKeyTest,WriteBufTest}.cpp | Concrete COW revision, WriteBuf and key policy; some generic factory checks | Move generic factory/error/dispatch witnesses to Slice first; retire COW-specific cases in #527 |
| tests/unittests/TestCOWChunkMetadata.hpp | COW private head/revision fake | Remove after common tests use typed Slice or mechanism-neutral doubles |
| tests/unittests/metadata/redis/RedisChunkMetadataTest.cpp | COW private CAS mixed with shared ChunkID allocation | Retire COW head/revision checks, retain allocation, invalid counter, ambiguous outcomes and cross-volume namespace tests under Slice |
| tests/unittests/metadata/ReclaimTest.cpp and tests/unittests/vfs/ReclaimerTest.cpp | COW typed physical delete and generic orphan/reclaim scheduling and retry | Remove COW physical GC cases in #527; retain generic metadata/non-revivability/idempotence using mechanism-neutral doubles. Never assert physical Slice GC before #273 |
| tests/unittests/metadata/redis/{RedisMetaImplReclaimTest,RedisMetaTxnReclaimTest}.cpp | Common reclaim/Redis integrity and historical COW cleanup | Preserve generic reclaim tests, retire only COW-specific private cleanup |
| tests/unittests/vfs/{FileReadWriterTest,FileHandleTest}.cpp and tests/unittests/fuse/VfsImplTest.cpp | Mixed COW fixtures and common read/write/flush/open/truncate/error semantics | FileHandleTest and VfsImplTest run with Slice; FileReadWriter now has independent Slice tests for cross-chunk dirty/cold reads, sparse EOF, writes, flush/retry, metadata failure, whole-file size commits and truncate; retire COW-private cases only after confirming equivalent shared-contract coverage |
| tests/unittests/volume/{VolumeImplTest,VolumeFormatTest}.cpp and tests/e2e/CommandCoverageTest.cpp | Common persisted identity, format/mount/config and COW override | Preserve enum values (cow=1, Slice=2, redis_cache=3), default Slice, format/mount failure. #527 adds legacy COW rejection |
| tests/unittests/VolumeRuntimeTestUtils.hpp and tests/unittests/metadata/redis/{RedisMetaImplTest,RedisMetaImplTestBase,RedisMetaTestSupport}.hpp | Previously COW-bound generic Redis and runtime fixtures | Common RedisMetaImpl tests now format Slice and bind its real deferred cleanup encoder; one explicit COW fixture owns the remaining private binary cleanup case. VolumeRuntimeTestUtils COW fallback remains Stage B cleanup |
| tests/unittests/metadata/redis/RedisSliceChunkMetadataTest.cpp and tests/unittests/chunk/slice/SliceChunkTest.cpp | Current Slice and typed factory contracts | Keep and extend unsupported/mismatch tests with synthetic generic metadata |
| tests/unittests/metadata/redis/{RedisMetaTxnNamespaceTest,RedisMetaTxnSetAttrTest}.cpp | Generic namespace and attribute mutation tests accidentally instantiated a COW cleanup encoder | Use the production Slice deferred-cleanup encoder through the shared test support; no concrete COW dependency |
| .github/workflows/ci.yml and scripts/conformance/ | Primary Slice required matrix, temporary COW smoke, historical trend | #527 removes COW smoke, preserves strict Slice writeback and historical trend provenance |

## Generic factory witness — initial migration

The COW Chunk test has a mixed FactoryRejectsInvalidRuntimeCompositionBeforeLookup
regression. SliceChunkTest.FactoryRejectsInvalidCompositionWithoutRelyingOnCOWFixtures
now independently covers missing runtime dependencies, wrong metadata type,
matching type without mechanism capability, unsupported future mechanism,
invalid mechanism and null output using only Slice/synthetic generic metadata.

## Generic witness migration — this PR

- `FileHandleTest`'s two metadata engines now expose the typed Slice metadata
  capability through `TestSliceChunkMetadata`. Shared handle lookup, policy,
  dirty-flush and inode-reclaim-fence contracts no longer depend on a COW
  volume or COW private metadata. This preserves observable lifecycle tests;
  it does not create or simulate physical Slice object deletion.
- `FileReadWriterTest` now has independent Slice `FileReadWriterSliceContractTest`
  and `FileReadWriterSliceSizeTest` suites. These protect dirty and persisted
  cross-chunk reads, offset/EOF behavior, sparse zero-filling, cold rewrites,
  validation, upload/metadata failure and retry, transient size, full-file
  size commits, truncate/grow sanitization and partial-output rollback.
  `MockFileMetadataEngine` now owns the shared mapping/size/transaction
  contract; `MockMetaEngine` is the derived COW-only private-head/revision
  fixture, and the Slice size suite instantiates the COW-free base.
  Slice-specific synchronization tests retain semantic completion barriers
  for foreground writes during blocked upload or metadata publication,
  truncate after a blocked Flush/Read, and stale GetAttr snapshots after a
  completed Flush. The Truncate tests additionally wait for a separately
  scheduled probe fiber after the attempted Truncate, so posting a pre-call
  baton alone cannot pass a blocking-order assertion. Two more Slice tests
  preserve independent concurrent reads while the first backend GET is held
  and ordered publication of two Flush generations while the older PUT is
  held; both verify persisted/read data and drain blocked fibers on failures.
  The Truncate barrier uses real mock mutation-entry accounting for both
  same-size SetAttr and typed CommitShrink/CommitGrow, rather than an inert
  counter: no mutation may start while the backend is held, and a completed
  Truncate must reach one of the actual metadata mutation paths. A separate
  Slice Read regression holds an older GetInode EOF snapshot across a
  completed Write+Flush and verifies the resumed read sees all committed
  bytes. A failed older Slice upload with concurrent newer writes also
  retains the merged dirty bytes and publishes them on retry; this preserves
  a COW-only generic failure/recovery witness before Stage B.
  These tests verify the logical FileReadWriter boundary
  without depending on COW revision mechanics.
  The original COW fixture remains intact for private revision/CAS tests and
  historical fault-injection comparison. Stage B must not delete its generic
  cases without retaining the behavior matrix under Slice or demonstrating
  independent equivalent witnesses.
- `VfsImplTest`'s shared FUSE hook and VFS integration mock now provides
  typed Slice metadata; the separate COW physical-GC/last-link fixture is
  intentionally untouched until #527. This preserves the common namespace,
  mount and callback error-path witnesses independently of COW.
- `RedisSliceChunkMetadataTest` now independently protects the shared ChunkID
  allocator: same-volume sequencing across metadata views, independent
  namespaces, invalid/missing/exhausted counter fail-closed behavior, and
  ambiguous Redis outcomes (including a later fresh ID after an applied but
  unacknowledged allocation). COW revision/head CAS remains COW-specific.
- Generic Redis transaction namespace/attribute tests use a real production
  Slice cleanup-work encoder, keeping those contracts independent of the
  retiring COW cleanup serializer. Generic raw RedisMetaTxn reclaim and Chunk
  transaction suites also use the real Slice encoder, preserving same-inode
  Link/Reclaim conflict arbitration, detached mapping/EOF atomicity, and
  known-partial-EXEC error semantics without COW-private cleanup payloads.
- `RedisMetaImplTestBase` now defaults to Slice, including common namespace,
  rename, Redis persistence/recovery, ACL, inode lifecycle, orphan and
  transaction contract tests across four test translation units. An explicit
  `RedisMetaImplCOWTest` owns the COW binary-layout-specific reclaim witness.
  Synthetic pending-delete intents in shared tests use Slice's encoder; the
  common Redis layer only validates the durable envelope, not private payloads.
  The shared header no longer includes COW-private helpers or treats a
  Slice ChunkID as a COW revision.
- The uncertain Redis ChunkID counter test now reads the committed counter
  from state and validates queued SET identities before persisting them. This
  prevents a scripted fake from manufacturing a successful new identity.
- Reclaimer's missing-engine and scan-failure regression tests now mount Slice
  where a volume is needed, exercise the common orphan/ChunkGcWorker
  scheduling contract, and use a synthetic cleanup participant only when
  testing the common scan error path. They never claim or enable Slice
  physical object reclamation.

## Full COW test deletion inventory

All **121** retiring `FileReadWriterTest` cases are accounted for in
[`526-cow-retirement-deletion-inventory.md`](526-cow-retirement-deletion-inventory.md),
with explicit priority mapping for high 64-bit offsets and partial/unknown
FileMetadata size commits. Six new COW-independent Slice tests now cover
these priority cases, including authoritative reconciliation, unreadable
readback and undetached-mapping fail-closed states. The remaining per-case
`V` and `E` classifications still require named-test equivalence confirmation
in Stage B; categorization is not permission to delete a generic contract.

## Stage B test-retirement review

The remaining COW-bound cases require the following deletion-time decisions:

| Mechanism ownership | Examples | Stage B action |
| --- | --- | --- |
| Generic FileReadWriter contracts | Reads across holes/EOF, size/offset errors, visible attributes, writeback/retry, truncate, multi-chunk reads, racing reads/writes | Preserve `FileReadWriterSliceContractTest`/`FileReadWriterSliceSizeTest`, COW-free `MockFileMetadataEngine`, and Slice VFS/FileHandle matrix; retain blocked PUT/metadata commit, real metadata mutation-entry Truncate barriers, independent concurrent reads, ordered Flush generations, stale GetAttr and Read-visible-EOF snapshots, and failed older PUT with concurrent newer writes. Stage B must classify remaining unique generic COW fault/race witnesses before any deletion |
| Concrete COW FileReadWriter semantics | Private head/revision CAS, immutable revision object keys, COW rewrite hydration and superseded revision cleanup | Remove COW-specific expectations and fixtures, do not translate revision counts into fabricated Slice semantics |
| Generic RedisMetaImpl transactions | Namespace/ACL/rename, FileMetadata atomicity, hard-link/orphan recovery, reclaim queue scans and aborts | Default RedisMetaImpl fixture now uses Slice; retain it and its generic contract tests |
| COW binary cleanup format | `RedisMetaImplCOWTest.TypedReclaimFreezesChunkIDAttachmentsInsteadOfLegacyDescriptors`, COW-private transaction encoders | Delete only format-specific assertions and COW encoders after ensuring generic frozen-work tests still run with Slice |
| Generic RedisMetaTxn reclaim/Chunk | FileMetadata mapping, typed size changes, authoritative transaction isolation, orphan serialization ordering | Already run with `SliceEncoderForTest`; retain these intact in Stage B; only COW-private payload assertions can be dropped |
| Volume, VFS and Reclaimer | Mechanism identity, ACL/operation callbacks, orphan scanning, generic cleanup scheduling and errors | Retain Slice/synthetic witnesses; remove only COW physical-object GC and temporary mounted COW smoke |

No stage is allowed to discard a generic contract without equivalent Slice or
mechanism-neutral behavior coverage. Slice physical GC is tracked by #273;
these deferred cleanup tests do not claim it is implemented.

## Boundaries and safety

Stage A does not disable COW, delete existing COW tests or drop COW mounted
smoke. Stage B atomically retires COW runtime and CI, and must add RED/GREEN
tests for rejecting previously persisted COW volumes, without interpreting
their data as Slice. Never misrepresent old COW physical GC tests as Slice GC
coverage: Slice physical cleanup remains disabled pending #273.

Primary Slice Debug/Release UT, mounted E2E, pjdfstest, ten-shard fstests,
and strict unexpected Flush/RELEASE writeback audit remain mandatory.
