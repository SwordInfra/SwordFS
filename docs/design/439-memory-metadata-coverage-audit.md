# Memory Metadata retirement: test-coverage inventory and cutover gates (#439)

**Baseline:** `origin/main` at `6a0cac5` (2026-10-09). This is an **audit** of the repository test specifications, not a claim that every test has been executed. The Memory backend is retained until #443. The authoritative test inventory is the table below; keep the original memory-only tests until all open C obligations have evidence.

## Decision and audit vocabulary

The production `IMetaEngine`/`MetaEngineRegistry` surface is preserved. Removing a second complete implementation must not remove backend-neutral behavior contracts or Redis-specific distributed correctness checks.

- **A — retire implementation-only assertion:** the asserted value is inherently defined by the Memory implementation rather than a portable contract; remove only at #443.
- **B — existing Redis evidence:** the named Redis test has a directly related behavioral assertion. A B mapping is **not** a byte-for-byte equivalence claim for every assertion in the Memory test; #443 must review residual edge cases rather than equating equal names with exhaustive parity. Keep the referenced Redis test.
- **C — coverage/equivalence evidence not established:** the row points to gap family **G01–G11**. #442 must confirm an existing assertion (cite exact source and check), extend a contract/Redis test, or document why its detail is Memory-private. C is a gate, **not** permission to delete a test and not proof that Redis currently has a bug.
- **D — external fixtures needing migration:** these are enumerated in the cross-tree table rather than artificially labelling a backend-owned test D; tracked by #440/#441.

This classification is intentionally conservative: cross-backend parity must be demonstrated by semantic assertions, not similar filenames. The original Memory cases remain as source of truth until migration.

## Coverage gaps grouped into actionable migration contracts

| Family | Scope and evidence required before #443 | Destination |
| --- | --- | --- |
| G01 | Legacy private chunk-index staging, independent cleanup, bridge binding, error/partial commit: demonstrate equivalent Redis semantic outcomes, not Memory transaction staging internals | #442; preserve #312/#394 |
| G02 | ChunkID uniqueness across volumes, detach/non-reuse and typed head lifecycle; test independently from legacy revisions | #442 |
| G03 | Directory cookies, invalid seek/peek, empty/mixed/large directory, independent iterators after mutation and invalid inode cases | #442; FUSE coverage #440 where useful |
| G04 | Rename/exchange topology, link-count bookkeeping, dot/dotdot, ancestry/cycles, missing parents, and **atomic multi-instance interleavings** | #442; Redis service-backed concurrency |
| G05 | Full POSIX access/default ACL canonicalization, malformed encoding, inheritance and umask; binary xattr atomic modes | #442; existing Redis ACL checks are partial evidence |
| G06 | Chunk CAS, sparse truncate, high indices, `off_t` bound/overflow and killpriv/setid edge combinations | #442; consult #312 / #394 |
| G07 | Atomic orphan publication/revival, current-revision reclaim, Link race, visitor abort/resume, delete-batch progress and cleanup safety | #442; reclaimer fixture migration #440 |
| G08 | Immutable/append-only policy invariants including ctime/idempotence, parent/entry combinations and exchange | #442 |
| G09 | Same-name creation, rename/swap, add/remove/list and reclaim concurrency; **Memory FiberMutex tests do not prove Redis cross-mount atomicity** | #442; #242 for later deterministic scheduling infrastructure |
| G10 | Primitive namespace transaction validation, nil outputs, missing/typed parents, ancestry and private transaction staging | #442; core contract where externally observable |
| G11 | API null/ENOENT/type-validation, basic POSIX behavior, sticky permissions, xattr metadata and execution-domain behavior | #442 |

**Important:** Assigning a family to #442 means *resolve it before approving #443* by identifying existing precise evidence or adding necessary checks; it does not automatically require 100+ new tests. If a behavior belongs to a different already-open issue (#238/#242 or #312/#394), cross-link its concrete verification and explicitly mark it blocking #443 only if necessary. Do not introduce a replacement production-grade in-memory backend.

## Per-test inventory

All **179** cases in eight Memory-only test files are listed below with their declaration lines from the baseline revision. `File.cpp:line` is relative to `tests/unittests/metadata/mem/`.

| ID | Original test | Kind | Evidence, unresolved family, or retirement reason |
| --- | --- | --- | --- |
| 001 | `ChunkPrivateMetadataTest.cpp:158` **ChunkMetadataBridgeTest.FactoryValidatesConstructionAndCOWSnapshotOutput** | C | **G01** → #442 |
| 002 | `ChunkPrivateMetadataTest.cpp:179` **MemMetaTxnBindingTest.ChunkMutationsFailClosedUntilBridgeIsBound** | B | `tests/unittests/metadata/redis/RedisMetaTxnChunkTest.cpp::ChunkOperationsFailClosedWithoutMetadataBridge` |
| 003 | `ChunkPrivateMetadataTest.cpp:201` **ChunkPrivateMetadataTest.ChunkIDAllocationIsIndependentOfLegacyFileMetadataRevisionTransactions** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::ChunkIDAllocationCoexistsWithLegacyChunkRevisionAllocator` |
| 004 | `ChunkPrivateMetadataTest.cpp:219` **MemMetaStoreBindingTest.OpensChunkMetadataCapabilityAndRejectsNullBridge** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::ChunkMetadataCompositionRejectsNullOutputsAndBridge` |
| 005 | `ChunkPrivateMetadataTest.cpp:234` **ChunkPrivateMetadataTest.PublishStagesMultiplePrivateRecordsWithPublicHead** | B | `tests/unittests/metadata/redis/RedisMetaTxnChunkTest.cpp::PrivateIndexPublicationCommitsAndRejectsWithLogicalHead` |
| 006 | `ChunkPrivateMetadataTest.cpp:287` **ChunkPrivateMetadataTest.TruncateLeavesLegacyPrivateRecordsForIndependentCleanup** | C | **G01** → #442 |
| 007 | `ChunkPrivateMetadataTest.cpp:328` **ChunkPrivateMetadataTest.LegacyTruncateExercisesCompleteSharedSizeLayoutContract** | B | `tests/unittests/metadata/redis/RedisMetaTxnChunkTest.cpp::TruncateExercisesCompleteSharedSizeLayoutContract` |
| 008 | `ChunkPrivateMetadataTest.cpp:369` **ChunkPrivateMetadataTest.ReclaimFreezesPrivateIndexAndRejectsBeforeInodeRemoval** | C | **G01** → #442 |
| 009 | `MemChunkMetadataTest.cpp:15` **MemCOWChunkMetadataTest.AllocatesMonotonicVolumeScopedChunkIDs** | B | `tests/unittests/metadata/redis/RedisChunkMetadataTest.cpp::AllocatesOneVolumeScopedIdentitySequenceAcrossTypedViews` |
| 010 | `MemChunkMetadataTest.cpp:28` **MemCOWChunkMetadataTest.IndependentVolumesStartWithIndependentIdentitySpaces** | C | **G02** → #442 |
| 011 | `MemChunkMetadataTest.cpp:40` **MemCOWChunkMetadataTest.StoresTypedHeadsWithPerChunkRevisionAllocationAndFullHeadCas** | B | `tests/unittests/metadata/redis/RedisChunkMetadataTest.cpp::StoresTypedHeadsWithPerChunkRevisionAllocationAndFullHeadCas` |
| 012 | `MemChunkMetadataTest.cpp:91` **MemCOWChunkMetadataTest.ErasedChunkStateDoesNotCauseChunkIdReuse** | C | **G02** → #442 |
| 013 | `MemChunkMetadataTest.cpp:114` **MemCOWChunkMetadataTest.RejectsInvalidTypedHeadOperationsAndUnsafeSameRevisionGrowth** | B | `tests/unittests/metadata/redis/RedisChunkMetadataTest.cpp::RejectsInvalidTypedHeadOperationsAndUnsafeSameRevisionGrowth` |
| 014 | `MemChunkMetadataTest.cpp:159` **MemCOWChunkMetadataTest.RuntimeAllocationRejectsThreadCaller** | B | `tests/unittests/metadata/redis/RedisChunkMetadataTest.cpp::RuntimeAllocationRejectsThreadCaller` |
| 015 | `MemMetaImplReadDirTest.cpp:70` **MemMetaImplReadDirTest.OpenDirEmpty** | C | **G03** → #442 |
| 016 | `MemMetaImplReadDirTest.cpp:86` **MemMetaImplReadDirTest.OpenDirWithEntries** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::OpenDirReturnsIndependentIteratorsAndSupportsSeek` |
| 017 | `MemMetaImplReadDirTest.cpp:109` **MemMetaImplReadDirTest.GetInodesReturnsAlignedPresentAndMissingResults** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::GetInodesReturnsAlignedPresentAndMissingResults` |
| 018 | `MemMetaImplReadDirTest.cpp:133` **MemMetaImplReadDirTest.OpenDirIteratorSupportsPeekAdvanceAndSeek** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::OpenDirReturnsIndependentIteratorsAndSupportsSeek` |
| 019 | `MemMetaImplReadDirTest.cpp:166` **MemMetaImplReadDirTest.OpenDirReturnsIndependentIterators** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::OpenDirReturnsIndependentIteratorsAndSupportsSeek` |
| 020 | `MemMetaImplReadDirTest.cpp:197` **MemMetaImplReadDirTest.OpenDirRejectsNonDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::ChunkAndOpenOperationsRejectWrongInodeTypes` |
| 021 | `MemMetaImplReadDirTest.cpp:209` **MemMetaImplReadDirTest.OpenDirNotADirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::ChunkAndOpenOperationsRejectWrongInodeTypes` |
| 022 | `MemMetaImplReadDirTest.cpp:222` **MemMetaImplReadDirTest.OpenDirMixedTypes** | C | **G03** → #442 |
| 023 | `MemMetaImplReadDirTest.cpp:247` **MemMetaImplReadDirTest.OpenDirAfterMove** | C | **G03** → #442 |
| 024 | `MemMetaImplReadDirTest.cpp:285` **MemMetaImplReadDirTest.OpenDirAfterUnlink** | C | **G03** → #442 |
| 025 | `MemMetaImplReadDirTest.cpp:299` **MemMetaImplReadDirTest.OpenDirLargeDirectory** | C | **G03** → #442 |
| 026 | `MemMetaImplRenameTest.cpp:57` **MemMetaImplRenameTest.BasicRenameFile** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 027 | `MemMetaImplRenameTest.cpp:70` **MemMetaImplRenameTest.RenameSourceNotFound** | C | **G04** → #442 |
| 028 | `MemMetaImplRenameTest.cpp:75` **MemMetaImplRenameTest.RenameRefusesDot** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 029 | `MemMetaImplRenameTest.cpp:80` **MemMetaImplRenameTest.RenameRefusesDotDot** | C | **G04** → #442 |
| 030 | `MemMetaImplRenameTest.cpp:93` **MemMetaImplRenameTest.RenameOverwriteFilePublishesVictimForReclaim** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 031 | `MemMetaImplRenameTest.cpp:129` **MemMetaImplRenameTest.RenameOverwriteEmptyDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameDirectoryOverEmptyDirectoryUpdatesSameParentNlink` |
| 032 | `MemMetaImplRenameTest.cpp:149` **MemMetaImplRenameTest.RenameDirectoryCrossDirectoryUpdatesNlink** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameDirectoryOverEmptyDirectoryUpdatesCrossParentNlink` |
| 033 | `MemMetaImplRenameTest.cpp:178` **MemMetaImplRenameTest.RenameDirectorySameDirectoryNlinkUnchanged** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameDirectoryOverEmptyDirectoryUpdatesSameParentNlink` |
| 034 | `MemMetaImplRenameTest.cpp:198` **MemMetaImplRenameTest.RenameDirectoryIntoSubtreeFails** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameRejectsOrdinaryTypeMismatchAndDirectoryCycles` |
| 035 | `MemMetaImplRenameTest.cpp:212` **MemMetaImplRenameTest.RenameFileOverDirectoryFails** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameRejectsOrdinaryTypeMismatchAndDirectoryCycles` |
| 036 | `MemMetaImplRenameTest.cpp:220` **MemMetaImplRenameTest.RenameDirectoryOverFileFails** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameRejectsOrdinaryTypeMismatchAndDirectoryCycles` |
| 037 | `MemMetaImplRenameTest.cpp:228` **MemMetaImplRenameTest.RenameExchangeFileAndDirectorySameParentSucceeds** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameExchangeDirectoryAndFileSupportsBothDirections` |
| 038 | `MemMetaImplRenameTest.cpp:244` **MemMetaImplRenameTest.RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology` |
| 039 | `MemMetaImplRenameTest.cpp:290` **MemMetaImplRenameTest.RenameExchangeRejectsCycleWhenSourceOnlyIsDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameExchangeRejectsCycleWhenOnlySourceIsDirectory` |
| 040 | `MemMetaImplRenameTest.cpp:308` **MemMetaImplRenameTest.RenameExchangeRejectsCycleWhenTargetOnlyIsDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameExchangeRejectsCycleWhenOnlyTargetIsDirectory` |
| 041 | `MemMetaImplRenameTest.cpp:326` **MemMetaImplRenameTest.RenameExchangeSameInodeAcrossParentsIsNoOp** | C | **G04** → #442 |
| 042 | `MemMetaImplRenameTest.cpp:357` **MemMetaImplRenameTest.RenameOverwriteNonEmptyDirectoryFails** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameRejectsNonEmptyTargetDirectory` |
| 043 | `MemMetaImplRenameTest.cpp:373` **MemMetaImplRenameTest.NlinkAccountingMultipleDirs** | C | **G04** → #442 |
| 044 | `MemMetaImplRenameTest.cpp:417` **MemMetaImplRenameTest.RenameDirectoryIntoItselfFails** | C | **G04** → #442 |
| 045 | `MemMetaImplRenameTest.cpp:434` **MemMetaImplRenameTest.RenameDirectoryIntoOwnSubtreeStillFails** | C | **G04** → #442 |
| 046 | `MemMetaImplRenameTest.cpp:448` **MemMetaImplRenameTest.RenameExchangeDirectoryIntoItselfFails** | C | **G04** → #442 |
| 047 | `MemMetaImplRenameTest.cpp:467` **MemMetaImplRenameTest.RenameExchangeWithAncestorDirectoryFails** | C | **G04** → #442 |
| 048 | `MemMetaImplTest.cpp:55` **MemMetaImplDomainTest.RuntimeApiRejectsThreadCaller** | C | **G11** → #442 |
| 049 | `MemMetaImplTest.cpp:188` **MemMetaImplTest.ConditionalUnlinkProtectsReplacedTemporaryEntry** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::ConditionalUnlinkProtectsReplacedTemporaryEntry` |
| 050 | `MemMetaImplTest.cpp:228` **MemMetaNoAtimeTest.OpenAndOpenDirSuppressOnlyImplicitAtimeUpdates** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::NoAtimeRuntimeBehaviorSuppressesImplicitOpenUpdates` |
| 051 | `MemMetaImplTest.cpp:259` **MemMetaImplTest.AllocateChunkRevisionIsMonotonicAndStartsAtOne** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::AllocateChunkRevisionIsMonotonicAndStartsAtOne` |
| 052 | `MemMetaImplTest.cpp:272` **MemMetaImplTest.StatFsReportsVirtualInodeCapacity** | A | Memory virtual-capacity StatFs computation (not a portable limit contract) → #443 |
| 053 | `MemMetaImplTest.cpp:288` **MemMetaImplTest.ReadlinkRejectsNullOutput** | C | **G11** → #442 |
| 054 | `MemMetaImplTest.cpp:297` **MemMetaImplTest.ReadlinkReturnsStoredTargetAndRejectsNonSymlinks** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::SymlinkHardLinkAndOpenBehaveLikePosixMetadata` |
| 055 | `MemMetaImplTest.cpp:313` **MemMetaImplTest.NamespaceOperationsRejectOverlongNameComponents** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::NamespaceOperationsRejectOverlongNameComponents` |
| 056 | `MemMetaImplTest.cpp:333` **MemMetaImplTest.MknodPersistsSupportedTypesModeAndDeviceIdentity** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MknodPersistsSupportedTypesModeAndDeviceIdentity` |
| 057 | `MemMetaImplTest.cpp:368` **MemMetaImplTest.CreateOwnershipUsesCallerGidWithoutParentSgid** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateOwnershipUsesCallerGidWithoutParentSgid` |
| 058 | `MemMetaImplTest.cpp:397` **MemMetaImplTest.CreateOwnershipInheritsGidAndDirectorySgidFromParent** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateOwnershipInheritsGidAndDirectorySgidFromParent` |
| 059 | `MemMetaImplTest.cpp:427` **MemMetaImplTest.CreateUnderSgidParentPreservesKernelAuthorizedSgidBit** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateUnderSgidParentPreservesKernelAuthorizedSgidBit` |
| 060 | `MemMetaImplTest.cpp:443` **MemMetaImplTest.BirthTimeSurvivesInodeIdentityAndAttributeMutations** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::BirthTimeSurvivesInodeIdentityAndAttributeMutations` |
| 061 | `MemMetaImplTest.cpp:472` **MemMetaImplTest.MknodReusesNamespaceValidationAndRejectsNonMknodTypes** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MknodReusesNamespaceValidationAndRejectsNonMknodTypes` |
| 062 | `MemMetaImplTest.cpp:488` **MemMetaImplTest.MknodSpecialNodesUseOrdinaryNamespaceLifecycle** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MknodSpecialNodesUseOrdinaryNamespaceLifecycle` |
| 063 | `MemMetaImplTest.cpp:529` **MemMetaImplTest.MetadataDoesNotDuplicateKernelDac** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MetadataDoesNotDuplicateKernelDac` |
| 064 | `MemMetaImplTest.cpp:546` **MemMetaImplTest.XAttrsProvideAtomicSetModesOrderedListingAndCtime** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::XAttrsMatchMemorySemanticsAndSurviveRemount` |
| 065 | `MemMetaImplTest.cpp:593` **MemMetaImplTest.PosixAccessAclValidatesCanonicalizesAndSynchronizesMode** | C | **G05** → #442 |
| 066 | `MemMetaImplTest.cpp:655` **MemMetaImplTest.PosixAclRejectsMalformedLinuxXattrEncoding** | C | **G05** → #442 |
| 067 | `MemMetaImplTest.cpp:726` **MemMetaImplTest.PosixDefaultAclControlsInheritanceAndUmask** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::PosixAclSemanticsMatchMemoryAndPersist` |
| 068 | `MemMetaImplTest.cpp:783` **MemMetaImplTest.XAttrLimitsApplyToDirectMetadataCallers** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::XAttrLimitsApplyToDirectMetadataCallers` |
| 069 | `MemMetaImplTest.cpp:803` **MemMetaImplTest.XAttrsFollowInodeIdentityUntilFinalReclaim** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::XAttrsFollowInodeIdentityUntilFinalReclaim` |
| 070 | `MemMetaImplTest.cpp:822` **MemMetaImplTest.StickyDirectoryOwnershipSafetyRemainsInMetadata** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::StickyDirectoryOwnershipSafetyRemainsInMetadata` |
| 071 | `MemMetaImplTest.cpp:848` **MemMetaImplTest.StickyDirectoryOwnerCanUnlinkEntry** | C | **G11** → #442 |
| 072 | `MemMetaImplTest.cpp:856` **MemMetaImplTest.StickyEntryOwnerCanUnlinkFromAnotherOwnersDirectory** | C | **G11** → #442 |
| 073 | `MemMetaImplTest.cpp:865` **MemMetaImplTest.RootCanUnlinkFromStickyDirectory** | C | **G11** → #442 |
| 074 | `MemMetaImplTest.cpp:875` **MemMetaImplTest.StickyEntryOwnerCanRemoveOwnDirectory** | C | **G11** → #442 |
| 075 | `MemMetaImplTest.cpp:888` **MemMetaImplTest.RenameNoReplaceSucceedsWhenTargetFree** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 076 | `MemMetaImplTest.cpp:904` **MemMetaImplTest.RenameNoReplaceFailsWhenTargetExists** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 077 | `MemMetaImplTest.cpp:923` **MemMetaImplTest.RenameExchangeSucceeds** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 078 | `MemMetaImplTest.cpp:944` **MemMetaImplTest.RenameExchangeFailsWhenTargetMissing** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 079 | `MemMetaImplTest.cpp:955` **MemMetaImplTest.RenameExchangeFileAndDirectoryAcrossParentsUpdatesTopology** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology` |
| 080 | `MemMetaImplTest.cpp:991` **MemMetaImplTest.TruncateNotFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MissingMetadataReturnsNotFoundConsistently` |
| 081 | `MemMetaImplTest.cpp:996` **MemMetaImplTest.TruncateUpdatesSizeWithoutImplicitSetidClearing** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::TruncateDoesNotInventKillpriv` |
| 082 | `MemMetaImplTest.cpp:1014` **MemMetaImplTest.TruncateSameSizeKeepsSuidSgid** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::TruncateDoesNotInventKillpriv` |
| 083 | `MemMetaImplTest.cpp:1034` **MemMetaImplTest.SetAttrSizeChangeDoesNotInventKillpriv** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrSizeAndOwnerChangesDoNotInventKillpriv` |
| 084 | `MemMetaImplTest.cpp:1051` **MemMetaImplTest.SetAttrOwnerChangeDoesNotInventKillpriv** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrSizeAndOwnerChangesDoNotInventKillpriv` |
| 085 | `MemMetaImplTest.cpp:1068` **MemMetaImplTest.SetAttrExplicitKillprivPreservesNonExecutableSgid** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrExplicitKillprivUsesModeAwareSgidRule` |
| 086 | `MemMetaImplTest.cpp:1082` **MemMetaImplTest.SetAttrExplicitKillprivClearsExecutableSgid** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrExplicitKillprivUsesModeAwareSgidRule` |
| 087 | `MemMetaImplTest.cpp:1096` **MemMetaImplTest.SetAttrExplicitKillprivLeavesOrdinaryModeUnchanged** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrExplicitKillprivUsesModeAwareSgidRule` |
| 088 | `MemMetaImplTest.cpp:1105` **MemMetaImplTest.SetAttrCombinedModeOwnerAndKillprivUsesFinalRequestedMode** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrCombinedModeOwnerAndKillprivUsesFinalRequestedMode` |
| 089 | `MemMetaImplTest.cpp:1125` **MemMetaImplTest.SetAttrLegacyExplicitModeIsNotSecondGuessed** | B | `tests/unittests/metadata/redis/RedisMetaTxnSetAttrTest.cpp::SetAttrLegacyExplicitModeIsNotSecondGuessed` |
| 090 | `MemMetaImplTest.cpp:1136` **MemMetaImplTest.CommitChunkInitialPublishIsIdempotentAndGrowsSizeMonotonically** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CommitChunkInitialPublishIsIdempotentAndGrowsSizeMonotonically` |
| 091 | `MemMetaImplTest.cpp:1169` **MemMetaImplTest.LoadChunkViewReturnsPublishedHeadAndCOWSnapshot** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::LoadChunkViewValidatesCOWSnapshot` |
| 092 | `MemMetaImplTest.cpp:1189` **MemMetaImplTest.CommitChunkRewriteUsesCompareAndSwapAndIsIdempotent** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CommitChunkRewriteUsesCompareAndSwapAndIsIdempotent` |
| 093 | `MemMetaImplTest.cpp:1233` **MemMetaImplTest.CommitChunkRejectsInvalidTargetsAndDescriptors** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CommitChunkRejectsInvalidTargetsAndDescriptors` |
| 094 | `MemMetaImplTest.cpp:1277` **MemMetaImplTest.ChunkMutationsRejectInvalidRevision** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::ChunkMutationsRejectInvalidRevision` |
| 095 | `MemMetaImplTest.cpp:1282` **MemMetaImplTest.PrepareReclaimMissingInodeIsNoOp** | C | **G07** → #442 |
| 096 | `MemMetaImplTest.cpp:1292` **MemMetaImplTest.UnlinkOnHardlinkedInodeKeepsInodeAlive** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::ReclaimKeepsLinkedInodesAndRemovesOrphans` |
| 097 | `MemMetaImplTest.cpp:1328` **MemMetaImplTest.OpenAcceptsUnlinkedButLiveInode** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::SymlinkHardLinkAndOpenBehaveLikePosixMetadata` |
| 098 | `MemMetaImplTest.cpp:1365` **MemMetaImplTest.ConcurrentRenameOverwriteHasNoObservableGap** | C | **G04** → #442 |
| 099 | `MemMetaImplTest.cpp:1423` **MemMetaImplTest.ConcurrentExchangeKeepsBothInodes** | C | **G04** → #442 |
| 100 | `MemMetaImplTest.cpp:1475` **MemMetaImplTest.UnlinkPublishesOrphanCandidateForLastLink** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::UnlinkPublishesDurableOrphanCandidate` |
| 101 | `MemMetaImplTest.cpp:1505` **MemMetaImplTest.HardlinkUnlinkKeepsInodeOffTheOrphanList** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::ReclaimKeepsLinkedInodesAndRemovesOrphans` |
| 102 | `MemMetaImplTest.cpp:1521` **MemMetaImplTest.RenameOverwritePublishesOrphanCandidate** | B | `tests/unittests/metadata/redis/RedisMetaImplRenameTest.cpp::RenameCoversMoveOverwriteNoReplaceAndExchange` |
| 103 | `MemMetaImplTest.cpp:1544` **MemMetaImplTest.LinkCancelsOrphanCandidate** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::LinkRevivesOrphanCandidateAndClearsMarker` |
| 104 | `MemMetaImplTest.cpp:1570` **MemMetaImplTest.PrepareReclaimFreezesAuthoritativeRevisionAndFencesLink** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::PrepareReclaimFreezesRecordAndRemovesLiveMetadata` |
| 105 | `MemMetaImplTest.cpp:1617` **MemMetaImplTest.PrepareReclaimUsesCurrentRevisionAfterRewrite** | C | **G07** → #442 |
| 106 | `MemMetaImplTest.cpp:1643` **MemMetaImplTest.PrepareReclaimRejectsLinkedInode** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::ReclaimKeepsLinkedInodesAndRemovesOrphans` |
| 107 | `MemMetaImplTest.cpp:1669` **MemMetaImplTest.ConcurrentReclaimAndLinkAreAtomic** | C | **G07** → #442 |
| 108 | `MemMetaImplTest.cpp:1747` **MemMetaImplTest.VisitorArgumentsAreValidated** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::VisitorArgumentsAreValidated` |
| 109 | `MemMetaImplTest.cpp:1761` **MemMetaImplTest.VisitorAbortStopsTheScanAndIsPropagated** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::ReclaimVisitorsVisitCurrentCandidatesAndPropagateAbort` |
| 110 | `MemMetaImplTest.cpp:1803` **MemMetaImplTest.OrphanVisitorResumesAfterAbortWithoutRevisitingCompletedCandidates** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::OrphanVisitorResumesAfterAbortWithoutRevisitingCompletedCandidates` |
| 111 | `MemMetaImplTest.cpp:1835` **MemMetaImplTest.PendingDeleteBatchVisitorAbortIsPropagated** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::PendingDeleteBatchVisitorAbortRetriesCurrentBufferedRecord` |
| 112 | `MemMetaImplTest.cpp:1859` **MemMetaImplTest.PendingDeleteBatchBoundsVisitsAndValidatesArguments** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::PendingDeleteBatchBoundsVisitsAndContinuesWhileQueueMutates` |
| 113 | `MemMetaImplTest.cpp:1901` **MemMetaImplTest.PendingDeleteBatchMakesProgressWithoutQueueMutation** | C | **G07** → #442 |
| 114 | `MemMetaImplTest.cpp:1928` **MemMetaImplTest.ImmutableAndAppendOnlyPolicyIsEnforcedInsideMetadataTransactions** | C | **G08** → #442 |
| 115 | `MemMetaImplTest.cpp:1964` **MemMetaImplTest.EffectiveInodeFlagChangesUpdateCtimeAndIdempotentWritesDoNot** | C | **G08** → #442 |
| 116 | `MemMetaImplTest.cpp:1986` **MemMetaImplTest.DirectoryPolicyDistinguishesPureAdditionFromRemovalOrReplacement** | C | **G08** → #442 |
| 117 | `MemMetaImplTest.cpp:2015` **MemMetaImplTest.RenameAndLinkPolicyChecksEveryParticipatingInode** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::RenameAndLinkPolicyChecksEveryParticipatingInode` |
| 118 | `MemMetaImplTest.cpp:2052` **MemMetaImplTest.RenameExchangePolicyChecksBothParentsAndBothEntries** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::RenameExchangePolicyChecksBothParentsAndBothEntries` |
| 119 | `MemMetaImplTest.cpp:2084` **MemMetaImplTest.RmDirPolicyChecksParentAndVictim** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::RmDirPolicyChecksParentAndVictim` |
| 120 | `MemMetaStoreConcurrencyTest.cpp:63` **MemMetaStoreConcurrencyTest.ConcurrentAddEntryNoDuplicate** | C | **G09** → #442 |
| 121 | `MemMetaStoreConcurrencyTest.cpp:102` **MemMetaStoreConcurrencyTest.ConcurrentAddEntrySameName** | C | **G09** → #442 |
| 122 | `MemMetaStoreConcurrencyTest.cpp:144` **MemMetaStoreConcurrencyTest.ConcurrentMoveEntryAtomicity** | C | **G09** → #442 |
| 123 | `MemMetaStoreConcurrencyTest.cpp:204` **MemMetaStoreConcurrencyTest.ConcurrentRemoveAndAdd** | C | **G09** → #442 |
| 124 | `MemMetaStoreConcurrencyTest.cpp:295` **MemMetaStoreConcurrencyTest.ConcurrentAddAndList** | C | **G09** → #442 |
| 125 | `MemMetaStoreConcurrencyTest.cpp:360` **MemMetaStoreConcurrencyTest.ConcurrentMoveToSameTarget** | C | **G09** → #442 |
| 126 | `MemMetaStoreSwapTest.cpp:52` **MemMetaStoreSwapTest.CrossDirectorySwap** | C | **G09** → #442 |
| 127 | `MemMetaStoreSwapTest.cpp:79` **MemMetaStoreSwapTest.SameDirectorySwapDifferentNames** | C | **G09** → #442 |
| 128 | `MemMetaStoreSwapTest.cpp:103` **MemMetaStoreSwapTest.SwapFileWithDirectory** | C | **G09** → #442 |
| 129 | `MemMetaStoreSwapTest.cpp:125` **MemMetaStoreSwapTest.SwapMissingSourceA** | C | **G09** → #442 |
| 130 | `MemMetaStoreSwapTest.cpp:136` **MemMetaStoreSwapTest.SwapMissingSourceB** | C | **G09** → #442 |
| 131 | `MemMetaStoreSwapTest.cpp:147` **MemMetaStoreSwapTest.SwapMissingParentA** | C | **G09** → #442 |
| 132 | `MemMetaStoreSwapTest.cpp:158` **MemMetaStoreSwapTest.SwapMissingParentB** | C | **G09** → #442 |
| 133 | `MemMetaStoreSwapTest.cpp:169` **MemMetaStoreSwapTest.ConcurrentSwapConsistency** | C | **G09** → #442 |
| 134 | `MemMetaStoreSwapTest.cpp:207` **MemMetaStoreSwapTest.SwapSameEntryNoOp** | C | **G09** → #442 |
| 135 | `MemMetaStoreSwapTest.cpp:225` **MemMetaStoreSwapTest.SwapDirectoriesCrossDirectory** | C | **G09** → #442 |
| 136 | `MemMetaStoreSwapTest.cpp:266` **MemMetaStoreSwapTest.SwapAcrossDifferentParentsUpdatesParentIno** | C | **G09** → #442 |
| 137 | `MemMetaStoreSwapTest.cpp:310` **MemMetaStoreSwapTest.SwapDirectoryIntoOwnSubtreeFails** | C | **G09** → #442 |
| 138 | `MemMetaStoreTest.cpp:79` **MemMetaStoreTest.ConstructorCreatesRoot** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::VolumeAndBasicLookupOperations` |
| 139 | `MemMetaStoreTest.cpp:90` **MemMetaStoreTest.PrivateChunkIndexReadsStagedWritesAndDropsRejectedChanges** | C | **G10** → #442 |
| 140 | `MemMetaStoreTest.cpp:128` **MemMetaStoreTest.PrivateChunkIndexRejectsInvalidHashAndOutputs** | B | `tests/unittests/metadata/redis/RedisMetaTxnChunkTest.cpp::PrivateChunkIndexPrimitivesValidateTheirPublicContract` |
| 141 | `MemMetaStoreTest.cpp:148` **MemMetaStoreTest.LookupInodeNotFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MissingMetadataReturnsNotFoundConsistently` |
| 142 | `MemMetaStoreTest.cpp:154` **MemMetaStoreTest.LookupInodeWithNullOut** | B | `tests/unittests/metadata/redis/RedisMetaTxnNamespaceTest.cpp::ReadPrimitivesValidateOutputs` |
| 143 | `MemMetaStoreTest.cpp:162` **MemMetaStoreTest.AddEntryCreatesFile** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateAndMkdirValidateNamesParentsAndDuplicates` |
| 144 | `MemMetaStoreTest.cpp:179` **MemMetaStoreTest.AddEntryCreatesDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateAndMkdirValidateNamesParentsAndDuplicates` |
| 145 | `MemMetaStoreTest.cpp:187` **MemMetaStoreTest.AddEntryAlreadyExists** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateAndMkdirValidateNamesParentsAndDuplicates` |
| 146 | `MemMetaStoreTest.cpp:194` **MemMetaStoreTest.AddEntryParentNotFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateAndMkdirValidateNamesParentsAndDuplicates` |
| 147 | `MemMetaStoreTest.cpp:199` **MemMetaStoreTest.AddEntryParentNotDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::CreateAndMkdirValidateNamesParentsAndDuplicates` |
| 148 | `MemMetaStoreTest.cpp:213` **MemMetaStoreTest.LookupEntryFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::VolumeAndBasicLookupOperations` |
| 149 | `MemMetaStoreTest.cpp:223` **MemMetaStoreTest.LookupEntryNotFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MissingMetadataReturnsNotFoundConsistently` |
| 150 | `MemMetaStoreTest.cpp:232` **MemMetaStoreTest.MoveEntrySuccess** | B | `tests/unittests/metadata/redis/RedisMetaTxnNamespaceTest.cpp::MoveEntryPersistsExplicitState` |
| 151 | `MemMetaStoreTest.cpp:256` **MemMetaStoreTest.MoveEntryOldParentNotFound** | C | **G10** → #442 |
| 152 | `MemMetaStoreTest.cpp:264` **MemMetaStoreTest.MoveEntryNewParentNotFound** | C | **G10** → #442 |
| 153 | `MemMetaStoreTest.cpp:272` **MemMetaStoreTest.MoveEntryTargetExists** | C | **G10** → #442 |
| 154 | `MemMetaStoreTest.cpp:287` **MemMetaStoreTest.MoveEntryOverwriteWorksWithoutResultOutput** | C | **G10** → #442 |
| 155 | `MemMetaStoreTest.cpp:316` **MemMetaStoreTest.UnlinkOnlyRemovesDirectoryEntry** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::UnlinkAndRmdirCoverSuccessAndTypeChecks` |
| 156 | `MemMetaStoreTest.cpp:348` **MemMetaStoreTest.UnlinkMissingEntryReturnsNotFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::UnlinkAndRmdirCoverSuccessAndTypeChecks` |
| 157 | `MemMetaStoreTest.cpp:357` **MemMetaStoreTest.UnlinkNonEmptyDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::UnlinkAndRmdirCoverSuccessAndTypeChecks` |
| 158 | `MemMetaStoreTest.cpp:370` **MemMetaStoreTest.UnlinkEmptyDirectory** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::UnlinkAndRmdirCoverSuccessAndTypeChecks` |
| 159 | `MemMetaStoreTest.cpp:388` **MemMetaStoreTest.ListEntriesSuccess** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::OpenDirReturnsIndependentIteratorsAndSupportsSeek` |
| 160 | `MemMetaStoreTest.cpp:399` **MemMetaStoreTest.ListEntriesEmptyDir** | C | **G10** → #442 |
| 161 | `MemMetaStoreTest.cpp:406` **MemMetaStoreTest.ListEntriesNotFound** | C | **G10** → #442 |
| 162 | `MemMetaStoreTest.cpp:416` **MemMetaStoreTest.IsDescendantOfDirectChild** | C | **G10** → #442 |
| 163 | `MemMetaStoreTest.cpp:424` **MemMetaStoreTest.IsDescendantOfGrandchild** | C | **G10** → #442 |
| 164 | `MemMetaStoreTest.cpp:436` **MemMetaStoreTest.IsDescendantOfNotDescendant** | C | **G10** → #442 |
| 165 | `MemMetaStoreTest.cpp:449` **MemMetaStoreTest.IsDescendantOfSelf** | C | **G10** → #442 |
| 166 | `MemMetaStoreTest.cpp:468` **MemMetaStoreTest.CommitChunkAndFindChunk** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::ChunkFindAndTruncateCoverSparseMetadata` |
| 167 | `MemMetaStoreTest.cpp:484` **MemMetaStoreTest.CommitChunkAndFindChunkPreserveIndexAboveUint32Max** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::SetAttrShrinkQueuesOnlyMaterializedSparseObjectsForCleanup` |
| 168 | `MemMetaStoreTest.cpp:499` **MemMetaStoreTest.CommitChunkConflictingInitialPublicationFails** | B | `tests/unittests/metadata/redis/RedisMetaTxnChunkTest.cpp::CommitChunkRejectsConflictingInitialPublication` |
| 169 | `MemMetaStoreTest.cpp:511` **MemMetaStoreTest.CommitChunkRejectsDerivedOffsetOverflow** | C | **G06** → #442 |
| 170 | `MemMetaStoreTest.cpp:528` **MemMetaStoreTest.CommitChunkAcceptsExtentAtOffTMaxAndRejectsBeyond** | C | **G06** → #442 |
| 171 | `MemMetaStoreTest.cpp:548` **MemMetaStoreTest.FindChunkNotFound** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::MissingMetadataReturnsNotFoundConsistently` |
| 172 | `MemMetaStoreTest.cpp:558` **MemMetaStoreTest.TruncateChunksNoChunksIsNoOp** | C | **G06** → #442 |
| 173 | `MemMetaStoreTest.cpp:565` **MemMetaStoreTest.TruncateChunksToZeroRemovesAllChunks** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::SetAttrShrinkRemovesAndClampsChunks` |
| 174 | `MemMetaStoreTest.cpp:586` **MemMetaStoreTest.TruncateChunksDropsChunksBeyondNewSize** | B | `tests/unittests/metadata/redis/RedisMetaImplTest.cpp::SetAttrShrinkRemovesAndClampsChunks` |
| 175 | `MemMetaStoreTest.cpp:610` **MemMetaStoreTest.TruncateChunksClampsStraddlingChunk** | B | `tests/unittests/metadata/redis/RedisMetaTxnChunkTest.cpp::TruncateClampsPersistedBoundaryChunk` |
| 176 | `MemMetaStoreTest.cpp:638` **MemMetaStoreTest.PrepareReclaimMissingInodeIsNoOp** | C | **G07** → #442 |
| 177 | `MemMetaStoreTest.cpp:649` **MemMetaStoreTest.ReclaimPrimitivesValidateOutputsAndRejectDirectories** | C | **G07** → #442 |
| 178 | `MemMetaStoreTest.cpp:662` **MemMetaStoreTest.PrepareReclaimFreezesWorkAndDropsOrphanedInode** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::PrepareReclaimFreezesRecordAndRemovesLiveMetadata` |
| 179 | `MemMetaStoreTest.cpp:716` **MemMetaStoreTest.PrepareReclaimKeepsLinkedInode** | B | `tests/unittests/metadata/redis/RedisMetaImplReclaimTest.cpp::ReclaimKeepsLinkedInodesAndRemovesOrphans` |

## Cross-tree Memory dependencies (fixture class D)

These are **not part of the 179 Memory-only cases** above. They are independently audited because removing `src/metadata/mem` would break the tests or silently change their setup. One row can represent many assertions; retain original test names until the target migration issue supplies replacement evidence.

| Consumer (baseline) | Required behavior / risk | Owner |
| --- | --- | --- |
| `tests/unittests/vfs/ReclaimerTest.cpp` | Direct `MemMetaImpl` inheritance (`StagedIntentMetaEngine`, `RawPendingDeleteMetaEngine`, `PendingReclaimMetaEngine`, `ForcedMultiPassMetaEngine`, `FailingScanMetaEngine`, `SlowOrphanScanMetaEngine`); controls malformed/staged reclaim records, visitor interruption and slow scan; several `MemCOWChunkMetadata` fixtures. Retain deterministic adversarial timing and completion barriers, not a renamed whole Memory backend. | #440 |
| `tests/unittests/fuse/VfsImplTest.cpp` | `ConfiguredMetaEngine<MemMetaImpl>` and direct stored pointer used by specific VFS scenarios; preserve FUSE-facing observable semantics. | #440 |
| `tests/unittests/vfs/FileReadWriterTest.cpp`, `tests/unittests/chunk/cow/COWChunkTest.cpp` | Direct `MemCOWChunkMetadata` test helpers for file IO and typed COW metadata; preserve head/ID semantics with narrow mocks or genuine Redis where needed. | #440 |
| `tests/unittests/vfs/FileHandleTest.cpp` | Direct `MemMetaImpl.hpp` include (inspect/remove as necessary); preserve handle lifecycle tests. | #440 |
| `tests/unittests/VolumeRuntimeTestUtils.hpp` | Global test registry engine is already an independent `swordfs-test-meta` with `ConfiguredMetaEngine`, but its fallback typed chunk metadata creates a `MemCOWChunkMetadata`; replace only the necessary test helper while retaining the thin engine injector. | #440 / #441 (coordinate owner) |
| `tests/unittests/volume/VolumeImplTest.cpp` | Many `memory://local` format/load/mount, argument-validation, storage URL and metadata runtime scenarios, plus `ConfiguredMetaEngine<MemMetaImpl>`; retain independent validation and persistent Redis lifecycle scenarios. | #441; coordinate specific subclass migration with #440 |
| `tests/unittests/volume/VolumeFormatTest.cpp` | Direct `metadata::mem::VolumeFile` storage tests: Memory-specific disk config format may retire, common volume-format validation must survive independently. | #441 |
| `tests/unittests/config/ConfigCenterTest.cpp`, `ValidatorTest.cpp` | CLI parse/option/URL fixtures rely on `memory://local`; switch to registered test URI or valid Redis URI without introducing unnecessary Redis IO; assert unsupported `memory://local` after cutover. | #441 |
| `tests/e2e/Fixture.cpp` | Main format/mount already **requires** `SWORDFS_METADATA_URL`; only `GetLimits()` still falls back to `memory://local` if env is absent. Remove fallback and fail explicitly. | #441 |
| `src/config/{Validator.cpp,ConfigCenter.cpp,ConfigCenter.hpp,Validator.hpp}`, `src/cmd/Format.cpp`, `src/metadata/IMetaEngine.hpp`, `src/storage/StorageUrl.hpp` | Production scheme validation, CLI examples, stale comments and Memory constant/registration; not test fixtures. Keep backend-neutral interface/registry and CLI contract while explicitly rejecting old scheme. | #443; architectural boundary #442 |
| `docs/design/{architecture.md,chunk-publication.md,production-test-coverage.md}` | Architecture and legacy documentation describe Memory backend as supported and reference its staging/transaction model; revise as part of cutover rather than allowing docs/implementation divergence. | #443 |

### Baseline test and CI evidence

- Memory inventory: **8 `.cpp` files, 179 declarations** (`TEST`, `TEST_F`, `FIBER_TEST`, or `FIBER_TEST_F`). It is intentionally not inferred from test-run counts. Redis test directory: **235 declarations across 20 `.cpp` files**, not one-for-one coverage proof.
- Unit runner: `scripts/testing/run-ut.sh` launches Redis via `docker compose up -d --wait redis`, sets `SWORDFS_REDIS_TEST_URL=redis://127.0.0.1:6379`, and runs `swordfs_test`. Redis service tests contain `GTEST_SKIP()` on missing env; the CI `build-and-test` job **explicitly fails if unit-test XML contains any `<skipped` entries**. Treat a direct local run without env as insufficient.
- E2E runner: `scripts/testing/run-e2e.sh` launches Redis and MinIO and sets `SWORDFS_METADATA_URL=redis://127.0.0.1:6379/15`. `Fixture::FormatVolume` and `Fixture::StartMount` explicitly require this env var.
- Conformance: `scripts/conformance/pjdfstest/run.sh` defaults to Redis URL (`.../14`); `scripts/conformance/fstests/run.sh` defaults to Redis URL (`.../13`). The authoritative fstests suite does not run against Memory (also noted in `docs/design/fstests-conformance.md`).
- Actual baseline workflow: **[CI run 37936538814](https://github.com/SwordInfra/SwordFS/actions/runs/37936538814)** on commit `6a0cac5` (main push, 2026-10-09) completed **successfully** with Debug/Release build-and-test, Release E2E, pjdfstest, and fstests aggregate jobs succeeding. This reports job status, **not** unpublished individual scenario results or parity for the 179 rows.
- `scripts/ci/docs_only.py` classifies docs-only PRs and skips expensive builds/conformance jobs. Consequently the #439 audit PR is verified as documentation, not falsely reported as a rerun of the full suite.

### Reproduction and final Redis-only gate

**Inventory sanity check (no compilation):**

```bash
python3 - <<'PY'
from pathlib import Path
import re
pattern = re.compile(r'\b(?:FIBER_TEST_F|FIBER_TEST|TEST_F|TEST)\s*\(')
for tree in ('mem', 'redis'):
    files = sorted(Path('tests/unittests/metadata', tree).glob('*.cpp'))
    total = sum(len(pattern.findall(p.read_text())) for p in files)
    print(tree, len(files), total)
PY
# Expected on 6a0cac5: mem 8 179; redis 20 235
```

**Before deleting Memory** (#443, with #439/#440/#441/#442 completed):

1. Resolve every `C` row: pinpoint a Redis/contract assertion or add the necessary test; decide Memory-private rows with a written rationale. Review `B` mappings against their source Memory assertions rather than interpreting a matching name as exhaustive parity. Ensure all `D` fixtures have observable-behavior replacements and no direct Memory source dependencies.
2. Preserve concurrency coverage at the real authority: Redis two-engine/multi-mount tests must demonstrate namespace mutation atomicity and no intermediate observable Rename/exchange state; a single-process Memory FiberMutex race is not equivalent.
3. Execute **formal GitHub CI** on the removal PR (non-doc changes cause full matrix): `build-and-test (Debug)`, `build-and-test (Release)`, `e2e-test (Release)`, `pjdfstest-conformance (Release)`, and `fstests-conformance (Release)` plus its shards. Do not substitute a local compile or an uncited green snapshot from a different SHA.
4. Reference commands for the CI runner environment after build: `bash scripts/testing/run-ut.sh --gtest_color=no --gtest_output=xml:unit-test-results.xml` (assert no skipped entries); `bash scripts/testing/run-e2e.sh --gtest_color=no`. The pjdfstest/fstests scripts require privileged FUSE/services and are run in formal CI; do not run them on the development host.
5. Diff review: `src/vfs`, `src/fuse`, `src/volume` and mechanism-neutral `src/chunk` code must remain independent from Redis concrete classes/keys/transactions; `IMetaEngine` and `MetaEngineRegistry` stay. Format/load/mount with Redis and unsupported `memory://local` rejection must be checked.
6. #443 PR must include the final coverage-disposition reconciliation (including numbers by kind and any post-audit changes), precise CI run links/SHAs, conformance caveats, and updated architecture docs. No unclassified or unassigned Memory test may be deleted.

### Task scope conclusion

The audit itself does **not** change product code, assertion content, execution behavior, or production support. It establishes a traceable removal protocol. The risky work (backend contract gap closure, fixture replacement and physical deletion) is already owned by #440–#443. Its correctness is not prematurely inferred from this documentation-only PR.
