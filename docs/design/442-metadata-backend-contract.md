# Metadata backend contract and dependency boundary (#442)

## Purpose

`IMetaEngine` is the filesystem-facing Metadata contract. Redis is the current
production implementation, but VFS, FUSE, Volume and mechanism-neutral Chunk
logic must not depend on Redis schemas, key names, client types, transactions or
concrete engine classes. Removing the historical Memory implementation (#443)
does not remove this abstraction.

## Ownership and test strategy

- `IMetaEngine` specifies portable namespace, inode, directory iteration,
  lifecycle and error semantics. Each future backend must pass the common
  behavior-level contract, not emulate Redis's implementation mechanics.
- A small contract harness accepts `IMetaEngine &`; the current adapter uses an
  actual formatted Redis volume and executes under the required fiber domain.
  New backends can run the same harness by supplying their own fixture.
- Backend-specific details (Redis WATCH/EXEC ambiguity, persistent key encoding,
  multi-client races, Redis COW allocation, corruption handling, recovery)
  remain in Redis-specific integration tests. Those tests are **not** replaced
  by portable tests, and tests of Redis distributed correctness must operate
  through independent engine instances against one authoritative volume.
- The #439 [audit](439-memory-metadata-coverage-audit.md) includes 69 C cases
  without fully demonstrated equivalent evidence. Their per-row disposition
  is required before #443 removes any Memory tests. A related test name alone
  is insufficient evidence.

## Mechanical dependency enforcement

The source check `scripts/static-analysis/check-metadata-boundaries.py` runs on
every CI PR/push in the always-running `clang-format` job. It rejects concrete
`metadata/redis/` includes, Redis metadata types/namespaces, and direct
`sw::redis`/hiredis client usage in `src/vfs`, `src/fuse`, `src/volume`, and
`src/chunk`. It intentionally allows
metadata-neutral types such as `IMetaEngine` and `ChunkType::kRedisCache`.

`src/config/Validator.cpp` is the only currently known explicit **configuration
parsing** exception: it validates a Redis URL through `RedisMetaConfig` after
checking `MetaEngineRegistry`. It does not expose the Redis client, keys or
transactions to VFS or the data path. This is a narrow current ownership
choice, not permission for other consumers to introduce concrete Redis logic.
The Memory scheme branch can be removed during #443; do not add a new factory or
parser abstraction for the sake of this audit.

The checker matches the Redis namespaces themselves, not merely specific
concrete type names, so namespace aliases cannot hide Redis client or metadata
imports. It also joins C/C++ backslash-continued logical lines before checking
preprocessor directives. This remains a deliberately narrow **direct source
dependency** check, not a transitive C++ include-graph or compiler AST audit.

## Semantics and limitations

- Namespace mutations must be externally atomic. A success must leave one
  authoritative namespace state; no observer can see a partially completed
  rename/exchange. A missing parent/name or incompatible type is rejected
  without changing unrelated entries.
- Directory cookies belong to the **individual opened iterator**; callers
  must not infer underlying Redis cursor or ordering behavior.
- Last-link unlink publishes an orphan candidate; reclaim only crosses its
  non-revivable boundary after checking nlink. A concurrent Link must win by
  reviving the inode **or** lose to reclaim; neither can resurrect detached
  metadata. Cleanup leakage after unexpected failure remains acceptable.
- Typed ChunkID identities must not be reused, and FileMetadata and private
  ChunkMetadata follow the independent consistency domains of #312/#394.
- Each backend independently owns its transaction and retry implementation.
  Passing a Redis-only contract suite neither proves a future backend correct
  nor substitutes for crash, outage, high-contention or POSIX conformance
  evidence. Future backends need new service-backed tests of their authority.

## Verification gates

`python3 scripts/static-analysis/check-metadata-boundaries.py` must pass in CI;
also validate the checker rejects representative forbidden imports and symbols.
Run Redis service-backed contract cases and all existing Redis tests through
`bash scripts/testing/run-ut.sh` in formal CI, with skipped tests treated as
failures, followed by applicable E2E, pjdfstest and fstests jobs. No local
SwordFS compilation is permitted by the engineering workflow. #443 remains
blocked on row-level coverage reconciliation and the separate #440/#441 fixture
migrations; this document is not a license to delete currently unmatched tests.

## Disposition of the 69 previously unresolved Memory-only C cases (#439 → #442)

The following is a **row-level removal gate**: a witness is a cited, real Redis
service-backed test that checks the relevant observable semantic outcome; an
internal-only case has a reasoned justification for not carrying forward the
Memory-only *implementation* assertion. It is **not** a claim of equivalent
lock-step internal algorithms. Every witness must run and pass in #442 CI
**before** #443 may use it as deletion evidence. New assertions in this PR
are identified by the `Contract...`, `Concurrent...`, or
`ReclaimUsesLatestPublishedRevisionAfterRewrite` names.

| #439 ID | Memory test | Disposition | Redis witness or reason |
| --- | --- | --- | --- |
| 001 (G01) | `ChunkMetadataBridgeTest.FactoryValidatesConstructionAndCOWSnapshotOutput` | Redis witness | `RedisMetaImplTest.ChunkMetadataCompositionRejectsNullOutputsAndBridge` — real Redis fixture binds typed COW bridge; factory and null/mismatched capability behavior |
| 006 (G01) | `ChunkPrivateMetadataTest.TruncateLeavesLegacyPrivateRecordsForIndependentCleanup` | Retire internal assertion | **Memory-only mechanism** — MemMetaTxn legacy private-record retention after truncate is a specific in-process staging detail; #394 separates private ChunkMetadata lifetime, and Redis tests assert authoritative detach and safe cleanup independently |
| 008 (G01) | `ChunkPrivateMetadataTest.ReclaimFreezesPrivateIndexAndRejectsBeforeInodeRemoval` | Redis witness | `RedisMetaTxnTest.PrepareReclaimScansAuthoritativeChunksInsideTransaction` — actual Redis transaction freezes authoritative ChunkIndex state before removing inode |
| 010 (G02) | `MemCOWChunkMetadataTest.IndependentVolumesStartWithIndependentIdentitySpaces` | Redis witness | `RedisCOWChunkMetadataTest.IndependentVolumesAndErasureNeverReuseAllocatedChunkIds` — independent Redis volume identity counters, typed erase and non-reuse |
| 012 (G02) | `MemCOWChunkMetadataTest.ErasedChunkStateDoesNotCauseChunkIdReuse` | Redis witness | `RedisCOWChunkMetadataTest.IndependentVolumesAndErasureNeverReuseAllocatedChunkIds` — independent Redis volume identity counters, typed erase and non-reuse |
| 015 (G03) | `MemMetaImplReadDirTest.OpenDirEmpty` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty, mixed-type, >100 entries, changed namespace and fresh iterator observations |
| 022 (G03) | `MemMetaImplReadDirTest.OpenDirMixedTypes` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty, mixed-type, >100 entries, changed namespace and fresh iterator observations |
| 023 (G03) | `MemMetaImplReadDirTest.OpenDirAfterMove` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty, mixed-type, >100 entries, changed namespace and fresh iterator observations |
| 024 (G03) | `MemMetaImplReadDirTest.OpenDirAfterUnlink` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty, mixed-type, >100 entries, changed namespace and fresh iterator observations |
| 025 (G03) | `MemMetaImplReadDirTest.OpenDirLargeDirectory` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty, mixed-type, >100 entries, changed namespace and fresh iterator observations |
| 027 (G04) | `MemMetaImplRenameTest.RenameSourceNotFound` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — missing source or destination parent leaves original source reachable |
| 029 (G04) | `MemMetaImplRenameTest.RenameRefusesDotDot` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — dotdot/self/descendant/exchange cycles fail before namespace mutation |
| 041 (G04) | `MemMetaImplRenameTest.RenameExchangeSameInodeAcrossParentsIsNoOp` | Redis witness | `RedisMetaImplTest.ContractCrossParentSameInodeExchangeIsNoOp` — same-inode exchange between distinct parent directories leaves both aliases, inode parent identity and nlink unchanged |
| 043 (G04) | `MemMetaImplRenameTest.NlinkAccountingMultipleDirs` | Redis witness | `RedisMetaImplTest.RenameDirectoryOverEmptyDirectoryUpdatesCrossParentNlink` — directory movement accounts parent nlink for cross-directory operations |
| 044 (G04) | `MemMetaImplRenameTest.RenameDirectoryIntoItselfFails` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — dotdot/self/descendant/exchange cycles fail before namespace mutation |
| 045 (G04) | `MemMetaImplRenameTest.RenameDirectoryIntoOwnSubtreeStillFails` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — dotdot/self/descendant/exchange cycles fail before namespace mutation |
| 046 (G04) | `MemMetaImplRenameTest.RenameExchangeDirectoryIntoItselfFails` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — dotdot/self/descendant/exchange cycles fail before namespace mutation |
| 047 (G04) | `MemMetaImplRenameTest.RenameExchangeWithAncestorDirectoryFails` | Redis witness | `RedisMetaImplTest.RenameExchangeRejectsCycleWhenOnlyTargetIsDirectory` — symmetric exchange cycle check rejects moving an ancestor into its descendant |
| 048 (G11) | `MemMetaImplDomainTest.RuntimeApiRejectsThreadCaller` | Redis witness | `RedisMetaImplTest.ContractRuntimeApiRejectsNonFiberCaller` — Debug domain assertion covers Redis runtime GetInode, replacing Memory-only domain assertion |
| 053 (G11) | `MemMetaImplTest.ReadlinkRejectsNullOutput` | Redis witness | `RedisMetaImplTest.ContractReadlinkAndReclaimValidateMissingState` — null symlink read output rejected, correct target returned |
| 065 (G05) | `MemMetaImplTest.PosixAccessAclValidatesCanonicalizesAndSynchronizesMode` | Redis witness | `RedisMetaImplTest.PosixAclSemanticsMatchMemoryAndPersist` — canonical default/access inheritance, mode synchronization and remount persistence |
| 066 (G05) | `MemMetaImplTest.PosixAclRejectsMalformedLinuxXattrEncoding` | Redis witness | `RedisMetaImplTest.ContractMalformedAclFailsWithoutMutation` — invalid ACL Linux encoding, duplicate entries, ordering, masks and disallowed default ACL |
| 071 (G11) | `MemMetaImplTest.StickyDirectoryOwnerCanUnlinkEntry` | Redis witness | `RedisMetaImplTest.ContractStickyDirectoryAllowsAuthorizedOwners` — file owner, directory owner and root exceptions and owner rmdir, following explicit unauthorised-user denial |
| 072 (G11) | `MemMetaImplTest.StickyEntryOwnerCanUnlinkFromAnotherOwnersDirectory` | Redis witness | `RedisMetaImplTest.ContractStickyDirectoryAllowsAuthorizedOwners` — file owner, directory owner and root exceptions and owner rmdir, following explicit unauthorised-user denial |
| 073 (G11) | `MemMetaImplTest.RootCanUnlinkFromStickyDirectory` | Redis witness | `RedisMetaImplTest.ContractStickyDirectoryAllowsAuthorizedOwners` — file owner, directory owner and root exceptions and owner rmdir, following explicit unauthorised-user denial |
| 074 (G11) | `MemMetaImplTest.StickyEntryOwnerCanRemoveOwnDirectory` | Redis witness | `RedisMetaImplTest.ContractStickyDirectoryAllowsAuthorizedOwners` — file owner, directory owner and root exceptions and owner rmdir, following explicit unauthorised-user denial |
| 095 (G07) | `MemMetaImplTest.PrepareReclaimMissingInodeIsNoOp` | Redis witness | `RedisMetaImplTest.ContractReadlinkAndReclaimValidateMissingState` — missing inode PrepareReclaim is a successful idempotent no-op |
| 098 (G04) | `MemMetaImplTest.ConcurrentRenameOverwriteHasNoObservableGap` | Redis witness | `RedisMetaImplTest.ConcurrentRenameOverwriteHasNoCrossEngineCreateGap` — independent Redis observer never sees an absent overwrite target |
| 099 (G04) | `MemMetaImplTest.ConcurrentExchangeKeepsBothInodes` | Redis witness | `RedisMetaImplTest.ConcurrentCrossEngineExchangeKeepsBothEntries` — two independent Redis metadata engines exchange same names concurrently; both names retain original identities |
| 105 (G07) | `MemMetaImplTest.PrepareReclaimUsesCurrentRevisionAfterRewrite` | Redis witness | `RedisMetaImplTest.ReclaimUsesLatestPublishedRevisionAfterRewrite` — latest published head, not obsolete pre-rewrite head, is frozen at reclaim |
| 107 (G07) | `MemMetaImplTest.ConcurrentReclaimAndLinkAreAtomic` | Redis witness | `RedisMetaTxnTest.LinkCommitInvalidatesConcurrentReclaimSnapshot` — Redis WATCH/EXEC retry proves link invalidates concurrent stale reclaim snapshot |
| 113 (G07) | `MemMetaImplTest.PendingDeleteBatchMakesProgressWithoutQueueMutation` | Redis witness | `RedisMetaImplTest.PendingDeleteBatchUncompletedIntentDoesNotStarveLaterRecords` — bounded scan advances over pending cleanup candidates without needing queue mutation |
| 114 (G08) | `MemMetaImplTest.ImmutableAndAppendOnlyPolicyIsEnforcedInsideMetadataTransactions` | Redis witness | `RedisMetaImplTest.ImmutableAndAppendOnlyPolicyMatchesMemoryBackend` — immutable/append-only mutation and attribute constraints enforced by Redis |
| 115 (G08) | `MemMetaImplTest.EffectiveInodeFlagChangesUpdateCtimeAndIdempotentWritesDoNot` | Redis witness | `RedisMetaImplTest.ContractInodeFlagIdempotencePreservesCtime` — effective flag change updates ctime, idempotent mutation does not |
| 116 (G08) | `MemMetaImplTest.DirectoryPolicyDistinguishesPureAdditionFromRemovalOrReplacement` | Redis witness | `RedisMetaImplTest.DirectoryInodePolicyMatchesMemoryBackend` — append-only directory addition versus prohibited mutation |
| 120 (G09) | `MemMetaStoreConcurrencyTest.ConcurrentAddEntryNoDuplicate` | Redis witness | `RedisMetaImplTest.ConcurrentIndependentNamesAcrossEnginesAreAllReachable` — disjoint concurrent namespace creates across two Redis engines leave all 40 entries reachable |
| 121 (G09) | `MemMetaStoreConcurrencyTest.ConcurrentAddEntrySameName` | Redis witness | `RedisMetaImplTest.ConcurrentNameCreationAcrossIndependentEnginesHasOneWinner` — same-name creation across independent Redis engines has one winner |
| 122 (G09) | `MemMetaStoreConcurrencyTest.ConcurrentMoveEntryAtomicity` | Redis witness | `RedisMetaImplTest.ConcurrentCrossEngineRenameOfSameSourceHasOneWinner` — two Redis engines race to move one source; exactly one rename takes effect |
| 123 (G09) | `MemMetaStoreConcurrencyTest.ConcurrentRemoveAndAdd` | Redis witness | `RedisMetaImplTest.ConcurrentUnlinkAndCreateAcrossEnginesCannotLoseSuccessfulCreate` — unlink/create collision preserves the semantics of both completed operations |
| 124 (G09) | `MemMetaStoreConcurrencyTest.ConcurrentAddAndList` | Retire internal assertion | **Memory-only mechanism** — MemMetaStore::ListEntries is a transactional memory snapshot; Redis directory iterators use documented weak cursor semantics, verified independently by ContractDirectoryIteratorsRemainIndependent, so the memory-only snapshot interleaving is not portable |
| 125 (G09) | `MemMetaStoreConcurrencyTest.ConcurrentMoveToSameTarget` | Redis witness | `RedisMetaImplTest.ConcurrentRenamesToSameDestinationKeepOneLiveWinner` — independent Redis engines publish to one target; final authoritative identity remains reachable |
| 126 (G09) | `MemMetaStoreSwapTest.CrossDirectorySwap` | Redis witness | `RedisMetaImplTest.RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology` — actual public exchange preserves identity, parent inode and directory topology |
| 127 (G09) | `MemMetaStoreSwapTest.SameDirectorySwapDifferentNames` | Redis witness | `RedisMetaImplTest.RenameExchangeDirectoryAndFileSupportsBothDirections` — same-directory symmetric exchange retains both names |
| 128 (G09) | `MemMetaStoreSwapTest.SwapFileWithDirectory` | Redis witness | `RedisMetaImplTest.RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology` — actual public exchange preserves identity, parent inode and directory topology |
| 129 (G09) | `MemMetaStoreSwapTest.SwapMissingSourceA` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — exchange with missing source fails and leaves the remaining source unchanged |
| 130 (G09) | `MemMetaStoreSwapTest.SwapMissingSourceB` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — exchange with missing target fails without removing the source |
| 131 (G09) | `MemMetaStoreSwapTest.SwapMissingParentA` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — exchange with nonexistent source parent is rejected |
| 132 (G09) | `MemMetaStoreSwapTest.SwapMissingParentB` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — rename with nonexistent target parent is rejected |
| 133 (G09) | `MemMetaStoreSwapTest.ConcurrentSwapConsistency` | Redis witness | `RedisMetaImplTest.ConcurrentCrossEngineExchangeKeepsBothEntries` — cross-engine exchange contention preserves both original inode identities |
| 134 (G09) | `MemMetaStoreSwapTest.SwapSameEntryNoOp` | Redis witness | `RedisMetaImplTest.RenameSameInodeThroughHardLinkIsNoOp` — identity-preserving no-op when source/target alias same inode |
| 135 (G09) | `MemMetaStoreSwapTest.SwapDirectoriesCrossDirectory` | Redis witness | `RedisMetaImplTest.RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology` — actual public exchange preserves identity, parent inode and directory topology |
| 136 (G09) | `MemMetaStoreSwapTest.SwapAcrossDifferentParentsUpdatesParentIno` | Redis witness | `RedisMetaImplTest.RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology` — actual public exchange preserves identity, parent inode and directory topology |
| 137 (G09) | `MemMetaStoreSwapTest.SwapDirectoryIntoOwnSubtreeFails` | Redis witness | `RedisMetaImplTest.RenameExchangeRejectsCycleWhenOnlySourceIsDirectory` — invalid exchange cycle rejected with namespace unchanged |
| 139 (G10) | `MemMetaStoreTest.PrivateChunkIndexReadsStagedWritesAndDropsRejectedChanges` | Retire internal assertion | **Memory-only mechanism** — private MemMetaTxn write staging and rollback is an implementation-specific mechanism; RedisMetaTxnTest.PrivateIndexPublicationCommitsAndRejectsWithLogicalHead protects observable Redis publication atomicity |
| 151 (G10) | `MemMetaStoreTest.MoveEntryOldParentNotFound` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — public rename missing-parent safety; no implicit Redis internal MoveEntry API |
| 152 (G10) | `MemMetaStoreTest.MoveEntryNewParentNotFound` | Redis witness | `RedisMetaImplTest.ContractNamespaceMissingParentsDoNotPartiallyMutate` — public rename missing-parent safety; no implicit Redis internal MoveEntry API |
| 153 (G10) | `MemMetaStoreTest.MoveEntryTargetExists` | Redis witness | `RedisMetaImplTest.RenameCoversMoveOverwriteNoReplaceAndExchange` — NoReplace collision is EEXIST without mutation |
| 154 (G10) | `MemMetaStoreTest.MoveEntryOverwriteWorksWithoutResultOutput` | Retire internal assertion | **Memory-only mechanism** — MemMetaStore::MoveEntry optional result out-pointer is private to the Memory transaction; public IMetaEngine::Rename returns a Status and tests assert the resulting identities |
| 160 (G10) | `MemMetaStoreTest.ListEntriesEmptyDir` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty directory emits dot entries and missing inode OpenDir returns NotFound |
| 161 (G10) | `MemMetaStoreTest.ListEntriesNotFound` | Redis witness | `RedisMetaImplTest.ContractDirectoryIteratorsRemainIndependent` — empty directory emits dot entries and missing inode OpenDir returns NotFound |
| 162 (G10) | `MemMetaStoreTest.IsDescendantOfDirectChild` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — the observable reason for MemMetaStore::IsDescendantOf is preventing nested namespace cycles; its internal boolean helper is not a public API |
| 163 (G10) | `MemMetaStoreTest.IsDescendantOfGrandchild` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — the observable reason for MemMetaStore::IsDescendantOf is preventing nested namespace cycles; its internal boolean helper is not a public API |
| 164 (G10) | `MemMetaStoreTest.IsDescendantOfNotDescendant` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — the observable reason for MemMetaStore::IsDescendantOf is preventing nested namespace cycles; its internal boolean helper is not a public API |
| 165 (G10) | `MemMetaStoreTest.IsDescendantOfSelf` | Redis witness | `RedisMetaImplTest.ContractRenameCannotCreateDirectoryCycle` — the observable reason for MemMetaStore::IsDescendantOf is preventing nested namespace cycles; its internal boolean helper is not a public API |
| 169 (G06) | `MemMetaStoreTest.CommitChunkRejectsDerivedOffsetOverflow` | Redis witness | `RedisMetaImplTest.ContractChunkPublicationHonorsOffTMaximum` — positive exact off_t upper boundary, beyond-limit and index overflow rejected |
| 170 (G06) | `MemMetaStoreTest.CommitChunkAcceptsExtentAtOffTMaxAndRejectsBeyond` | Redis witness | `RedisMetaImplTest.ContractChunkPublicationHonorsOffTMaximum` — positive exact off_t upper boundary, beyond-limit and index overflow rejected |
| 172 (G06) | `MemMetaStoreTest.TruncateChunksNoChunksIsNoOp` | Redis witness | `RedisMetaImplTest.ContractChunkPublicationHonorsOffTMaximum` — truncate of an inode without any attached chunks remains a success |
| 176 (G07) | `MemMetaStoreTest.PrepareReclaimMissingInodeIsNoOp` | Redis witness | `RedisMetaImplTest.ContractReadlinkAndReclaimValidateMissingState` — missing inode PrepareReclaim is a successful idempotent no-op |
| 177 (G07) | `MemMetaStoreTest.ReclaimPrimitivesValidateOutputsAndRejectDirectories` | Retire internal assertion | **Memory-only mechanism** — Memory-only MemMetaStore transaction API tests internal output parameter and directory rejection; service-facing IMetaEngine reclaim obeys nlink and is covered by RedisMetaImplTest.ReclaimKeepsLinkedInodesAndRemovesOrphans |

**Scope of these dispositions:** Memory backend-specific store/transaction
assertions are not forcibly reproduced as public interfaces. The public
contract, namespace/reclaim state and cross-engine behavior are verified
against real Redis instead. The original 109 B rows of #439 still need their
final assertion-preservation review during #443, and #440/#441 fixture
migrations remain required. #442 itself does not delete any Memory test.

**Parity limitations:** A test exercising two independent RedisMetaImpl
instances proves the recorded interleaving outcome under its test schedule,
not all possible interleavings or failure schedules. Broad deterministic
fault scheduling is #242, not a hidden prerequisite implemented here.
