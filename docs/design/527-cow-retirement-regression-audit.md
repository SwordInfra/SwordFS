# #527 — verified COW FileReadWriter test-retirement audit

The 121 Stage A COW-fixture cases were assessed **by contract ownership**, not by
attempting one-for-one COW test duplication. The Stage A `B`/`V`/`E`/`P` labels are
historical triage, **not migration instructions**. In this Stage B disposition:

- `direct`: a meaningful, currently supported **mechanism-neutral contract**
  (POSIX reads/writes/EOF, metadata failure, durable visibility, concurrency,
  reclaim safety) has a named Slice or VFS test. The exact COW procedure or
  private data layout is **not** part of the contract.
- `adapted`: a mixed old test had a common safety property, but its COW-specific
  identity/head/revision/GC/retry expectations do **not** apply to Slice. The
  named test is an independent Slice design invariant, *not* a demand for
  equivalent COW internals or identical error codes.
- `retired-private`: the original test is **COW-implementation-specific and
  requires no Slice replacement**. Any related independent Slice test mentioned
  in the note is context only, not a migration obligation or evidence of
  test-by-test equivalence. No new test may be added solely for this old case.

The `Slice/VFS witness (if needed)` column says `— (not required)` for
`retired-private`. The six Stage A `B` generic contracts remain protected.
Review original failure injection and observable results only insofar as they
express a **current Slice or generic filesystem invariant**. Avoid retaining
COW's immutable revision counting, private head CAS, snapshot/hydration
implementation, object-key choices, competing-writer error codes, or physical
GC/delete/retry policy. Existing E2E/pjdfstest/fstests additionally protect
public behavior but do not require COW-specific fixture shapes.

**All-case classification:** 74 generic `direct`, 13 mixed `adapted`, 34
`retired-private` (not ported). The scope remains 121/121; an existing Slice
fixture's name is never sufficient by itself to justify a migration.

| # | Former COW case | Stage A | Decision | Slice/VFS witness (if needed) | Contract / COW-private retirement boundary |
|---:|---|:---:|---|---|---|
| 1 | `FullChunk` | E | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 2 | `WithinChunkWithOffset` | E | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 3 | `PartialChunkAtEOF` | E | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 4 | `PersistedPartialChunkAtEOFReturnsShortRead` | V | direct | `FileReadWriterSliceContractTest.SparseWriteAndColdReadPreserveZerosAcrossChunks` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 5 | `PastEOF` | E | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 6 | `CrossChunkBoundary` | E | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 7 | `CrossChunkExactBoundary` | E | direct | `FileReadWriterSliceContractTest.ExactChunkBoundaryReadEndsWithoutFetchingNextChunk` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 8 | `CrossChunkReadsIntoSecondChunk` | E | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 9 | `CrossChunkExhaustsSecondChunk` | V | direct | `FileReadWriterSliceContractTest.DirtyCrossChunkReadsRespectOffsetAndLogicalEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 10 | `CrossChunkSecondChunkMissing` | E | direct | `FileReadWriterSliceContractTest.SparseWriteAndColdReadPreserveZerosAcrossChunks` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 11 | `ZeroSizeRequest` | E | direct | `FileReadWriterSliceContractTest.InvalidOffsetsAndEmptyReadsCannotPublishData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 12 | `NegativeReadOffsetIsRejected` | E | direct | `FileReadWriterSliceContractTest.InvalidOffsetsAndEmptyReadsCannotPublishData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 13 | `ZeroLengthNegativeReadOffsetIsRejected` | E | direct | `FileReadWriterSliceContractTest.InvalidOffsetsAndEmptyReadsCannotPublishData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 14 | `NegativeWriteOffsetIsRejectedBeforeChunkLookup` | E | direct | `FileReadWriterSliceContractTest.InvalidOffsetsAndEmptyReadsCannotPublishData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 15 | `HighOffsetWriteFlushAndFreshReadPreserveLogicalIndex` | B | direct | `FileReadWriterSliceSizeTest.HighOffsetWriteFlushAndColdReadPreserve64BitChunkIndex` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 16 | `WritePastSupportedFileSizeIsRejectedBeforeMutation` | V | direct | `FileReadWriterSliceSizeTest.OverflowingWriteDoesNotMutateMetadataOrUploadData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 17 | `VisibleSizeMetadataFailurePropagatesFromRead` | V | direct | `FileReadWriterSliceSizeTest.InodeMetadataReadFailurePropagatesWithoutReturningSparseBytes` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 18 | `ReadRejectsPersistedSizeBeyondSupportedFileRange` | V | direct | `FileReadWriterSliceSizeTest.CorruptedPersistedEofFailsClosedBeforeChunkRead` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 19 | `VisibleSizeSnapshotCannotRegressBehindCompletedFlush` | E | direct | `FileReadWriterSliceSizeTest.StaleInodeSnapshotCannotRegressReadEofAfterCompletedFlush` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 20 | `EmptyOutputOnNoData` | E | direct | `FileReadWriterSliceContractTest.InvalidOffsetsAndEmptyReadsCannotPublishData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 21 | `MissingChunkMetadataReadsAsSparseZeros` | E | direct | `FileReadWriterSliceContractTest.SparseWriteAndColdReadPreserveZerosAcrossChunks` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 22 | `MaterializedPublishedChunkZeroFillsItsUncoveredTail` | V | direct | `FileReadWriterSliceContractTest.SparseWriteAndColdReadPreserveZerosAcrossChunks` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 23 | `PersistedChunkShortReadFailsClosed` | V | direct | `SliceChunkTest.WindowCacheRejectsShortCommittedObjectWithoutPublication` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 24 | `PersistedChunkReadErrorRollsBackPartialOutput` | P | retired-private | — (not required) | COW physical error rollback retired; Slice completed read spans are not returned on later failed GET; related independent Slice coverage: `SliceChunkTest.LaterPhysicalGetFailureLeavesEarlierCompletedSpanUnpublished` (not a migrated COW test) |
| 25 | `PersistedChunkZeroLengthReadIsNoOp` | P | retired-private | — (not required) | COW empty read short-circuit retired; supported FileReadWriter zero-length read tested; related independent Slice coverage: `FileReadWriterSliceContractTest.InvalidOffsetsAndEmptyReadsCannotPublishData` (not a migrated COW test) |
| 26 | `ChunkMetadataIOErrorPropagatesFromRead` | E | direct | `FileReadWriterSliceContractTest.MetadataLookupFailurePropagatesWithoutInventingSparseData` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 27 | `MalformedChunkMetadataPropagatesFromRead` | E | direct | `SliceChunkTest.CorruptPublishedHistoryAndFailedSnapshotAreHardErrors` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 28 | `ChunkMetadataIOErrorPropagatesFromWrite` | V | direct | `FileReadWriterSliceContractTest.FailedMetadataLookupPreventsWriteAcceptance` | A failed metadata read prevents accepting a write and leaves no published object; healthy retries succeed |
| 29 | `CrossChunkMetadataErrorDrainsSubmittedReadBeforeReturning` | E | direct | `FileReadWriterSliceSizeTest.CrossChunkMetadataErrorCannotReturnPartialRead` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 30 | `SparseReadWithMultipleHoles` | E | direct | `FileReadWriterSliceContractTest.SparseWriteAndColdReadPreserveZerosAcrossChunks` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 31 | `FlushRetriesPutFailureUntilDataIsPublished` | E | direct | `FileReadWriterSliceContractTest.FailedFlushRetainsDirtyBytesAndCanRetry` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 32 | `PutFailureRetryPublishesNewTypedAttachment` | E | direct | `SliceChunkTest.DefinitePublicationFailureRetainsDirtyAndPreparedObjectForRetry` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 33 | `SuccessfulFirstFlushIsIdempotentWithoutAdditionalWrites` | V | direct | `FileReadWriterSliceContractTest.RepeatedCleanFlushDoesNotRepublishImmutableObjects` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 34 | `FlushRetriesRevisionAllocationFailureBeforeUploadingObject` | P | retired-private | — (not required) | COW revision allocation retired; Slice ChunkID/record allocation rejects failure before publication; related independent Slice coverage: `SliceChunkTest.AllocationFailureAndBadSliceIdentityNeverPutOrPublish` (not a migrated COW test) |
| 35 | `CompetingInitialPublishersCannotReplaceWinningTypedAttachment` | V | adapted | `SliceChunkTest.FirstAttachLoserRebasesOntoTheWinningIndex` | Slice first-attachment loser rebases to winner rather than returning COW head conflict |
| 36 | `FlushRetriesPublicationFailureWithFreshRevision` | P | retired-private | — (not required) | COW revision retry retired; Slice record reuses confirmed publication, never duplicates an uncertain append; related independent Slice coverage: `SliceChunkTest.FailedFileMetadataFinalizeRetriesWithoutDuplicateSliceAppend` (not a migrated COW test) |
| 37 | `AmbiguousUncommittedTypedAttachmentRetriesWithFreshChunkIdentity` | V | adapted | `SliceChunkTest.UnknownFirstAttachWithAnotherWinnerNeedsExplicitRebase` | Slice unknown attach with winner requires authoritative rebase, not COW revision retry |
| 38 | `DirtyUnattachedWriterRejectsCompetingTypedAttachment` | V | adapted | `SliceChunkTest.FirstAttachLoserRebasesOntoTheWinningIndex` | Slice reconciles to winning index rather than carrying COW unattached dirty head |
| 39 | `RejectedTypedAttachmentRetryUsesFreshImmutableIdentity` | V | adapted | `SliceChunkTest.FailedDefinitePutUsesFreshKeyOnRetryWithoutRemovingLocalDirtyData` | Slice retries with fresh immutable SliceID, not private COW revision |
| 40 | `FirstAttachmentUnknownSizeOnlyDoesNotReplayCandidate` | V | adapted | `SliceChunkTest.UnknownPartialFirstAttachMustNotRetireDirtyUntilEofFinalized` | Slice partial-EXEC first attach cannot retire dirty bytes until EOF confirmed |
| 41 | `FirstAttachmentUnknownMappingOnlyFailsClosed` | V | adapted | `SliceChunkTest.UnknownFirstAttachWithoutConfirmedMappingDoesNotAcknowledgeFlush` | Slice unresolved attachment has no safe publication acknowledgment |
| 42 | `FirstAttachmentKnownExecCommandFailureDoesNotRepairOrReplay` | V | adapted | `SliceChunkTest.UnknownPartialFirstAttachMustNotRetireDirtyUntilEofFinalized` | Known partial command error uses the same fail-closed rule as unknown first-attachment partial commit |
| 43 | `CachedCleanReadUsesLatestPrivateHeadAfterExternalRewrite` | P | retired-private | — (not required) | COW private-head cache retired; ordered Slice records determine authoritative bytes; related independent Slice coverage: `SliceChunkTest.OrderedPublishedOverwritesAndZeroCoversWinByCommitOrder` (not a migrated COW test) |
| 44 | `MissingCurrentPrivateObjectIsNotMistakenForGcRace` | P | retired-private | — (not required) | COW object-GC race retired; mapped missing Slice index is a hard error; related independent Slice coverage: `SliceChunkTest.FailurePreservesOutputAndMissingMappedIndexIsNotSparse` (not a migrated COW test) |
| 45 | `FirstAttachmentRetriesWhenEofBoundaryAttachesWithoutSizeChange` | V | adapted | `SliceChunkTest.TransientEofConflictRetriesWithoutReappendingTheSameSlice` | Slice EOF conflict retries conditional finalize without duplicate record append |
| 46 | `SuccessfulTypedRewritesQueueEverySupersededRevision` | P | retired-private | — (not required) | COW superseded revision queue retired; no physical Slice object GC before #273; related independent Slice coverage: `SliceChunkTest.OrderedPublishedOverwritesAndZeroCoversWinByCommitOrder` (not a migrated COW test) |
| 47 | `RejectedTypedRewriteRetriesWithFreshPrivateRevision` | P | retired-private | — (not required) | COW head CAS revision retired; Slice append failure retains authoritative content; related independent Slice coverage: `SliceChunkTest.FailedFileMetadataFinalizeRetriesWithoutDuplicateSliceAppend` (not a migrated COW test) |
| 48 | `FlushNeverShrinksExistingFileSize` | V | direct | `FileReadWriterSliceSizeTest.LowerOffsetRewriteFlushNeverShrinksPreviouslyCommittedEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 49 | `SuccessfulFlushClearsTransientLiveSize` | V | direct | `FileReadWriterSliceSizeTest.SuccessfulFlushRetiresTransientSizeOverlay` | After successful Flush, an external authoritative EOF decrease must not be masked by stale dirty live size |
| 50 | `GetAttrSnapshotCannotRegressReadEOFBehindCompletedFlush` | E | direct | `FileReadWriterSliceSizeTest.CompletedFlushCannotRegressConcurrentGetAttrSnapshot` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 51 | `LiveAttrGuardUsesTransientSize` | V | direct | `FileHandleTest.GetAttrReflectsUnflushedWriteSize` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 52 | `GetAttrBlocksDoNotPublishCachedWrite` | V | direct | `VfsImplIntegrationTest.GetAttrReportsBlocksForCachedUnflushedWrite` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 53 | `SparseCachedWriteKeepsBlocksBelowLogicalSize` | V | direct | `FileReadWriterSliceSizeTest.SparseCachedWriteReportsBlocksWithoutPublishing` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 54 | `FailedFlushKeepsTransientLiveSize` | E | direct | `FileReadWriterSliceSizeTest.FailedFlushKeepsUncommittedLiveSizeVisible` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 55 | `SuccessfulTruncateDoesNotLeaveTransientSizeOverlay` | V | direct | `FileReadWriterSliceSizeTest.SuccessfulTruncateRetiresTransientSizeOverlay` | A successful Truncate clears dirty EOF overlay; later authoritative EOF decrease is visible to GetAttr |
| 56 | `FlushContinuesOtherChunksAfterOneChunkFails` | V | direct | `FileReadWriterSliceContractTest.FailedChunkUploadDoesNotPreventIndependentChunkPublication` | Assert one surviving attachment and cold-reader content *before* retry, then complete retry |
| 57 | `AmbiguousTypedFirstAttachmentReconcilesBeforeTruncate` | V | direct | `FileReadWriterSliceSizeTest.AmbiguousFirstAttachmentReconcilesBeforeTruncate` | Ambiguous committed Slice attachment is confirmed, then truncate and grow must return zeros in discarded tail; no COW head/revision assertions |
| 58 | `WriteAfterPartialTruncateZeroFillsDiscardedRange` | E | direct | `FileReadWriterSliceSizeTest.TruncateAndGrowNeverReviveDiscardedSliceSuffix` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 59 | `UnflushedWriteVisibleAcrossHandles` | V | direct | `FileReadWriterSliceContractTest.WritesAreVisibleAcrossOpenHandlesBeforeAndAfterFlush` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 60 | `FlushedDataVisibleAcrossHandles` | V | direct | `FileReadWriterSliceContractTest.WritesAreVisibleAcrossOpenHandlesBeforeAndAfterFlush` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 61 | `WriteAfterFlushOverwritesExistingChunkWithoutLosingUntouchedBytes` | E | direct | `FileReadWriterSliceContractTest.FlushedRewriteKeepsUntouchedBytesForColdReader` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 62 | `ReopenedWriterAppendsWithinExistingFlushedChunk` | E | direct | `FileReadWriterSliceContractTest.ReopenedWriterAppendsWithinPreviouslyFlushedChunk` | A new FileReadWriter appends after the previous committed EOF and a cold reader observes extended content |
| 63 | `TypedRewriteAdvancesPrivateHeadAndRetainsImmutableObjectVersions` | P | retired-private | — (not required) | COW head revision/count retired; Slice ordered immutable record overrides are authoritative; related independent Slice coverage: `SliceChunkTest.OrderedPublishedOverwritesAndZeroCoversWinByCommitOrder` (not a migrated COW test) |
| 64 | `RewritePutFailureKeepsOldVersionAuthoritativeAndCanRetry` | V | direct | `SliceChunkTest.FailedOrAmbiguousPutKeepsDirtyBytesAndRetriesWithFreshSliceIdentity` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 65 | `TypedRewriteAfterAmbiguousFinalizationPublishesFreshPrivateRevision` | P | retired-private | — (not required) | COW revision reallocation retired; Slice finalization must not duplicate published record; related independent Slice coverage: `SliceChunkTest.LostAttachedInodeFinalizeReplyPreservesRecordForSafeRetry` (not a migrated COW test) |
| 66 | `AmbiguousRewriteRetryFailureKeepsLatestDataWritable` | P | retired-private | — (not required) | COW retry head CAS retired; Slice failed/unknown commit retains data and refuses unsafe replay; related independent Slice coverage: `SliceChunkTest.LostAttachedInodeFinalizeReplyPreservesRecordForSafeRetry` (not a migrated COW test) |
| 67 | `TypedRewriteRetryFinalizesEofAfterSuccessfulPrivateHeadCas` | P | retired-private | — (not required) | COW private head finalization retired; Slice EOF conditional finalization is separately tested; related independent Slice coverage: `SliceChunkTest.LostAttachedInodeFinalizeReplyPreservesRecordForSafeRetry` (not a migrated COW test) |
| 68 | `TypedRewriteRejectsAnExternallyAdvancedPrivateHeadWithoutDeletingIt` | P | retired-private | — (not required) | COW head-race CAS retired; Slice authoritative ChunkID replacement must not be masked; related independent Slice coverage: `SliceChunkTest.BoundaryClampDoesNotMaskReplacedAuthoritativeChunkId` (not a migrated COW test) |
| 69 | `RewriteHydrationPropagatesBackendReadFailure` | P | retired-private | — (not required) | COW full-chunk hydration retired; Slice RMW read errors must not publish zeros; related independent Slice coverage: `SliceChunkTest.WindowCachePhysicalReadFailureDoesNotPublishStaleZeros` (not a migrated COW test) |
| 70 | `RewriteHydrationRejectsObjectShorterThanDescriptor` | P | retired-private | — (not required) | COW descriptor hydration retired; Slice short committed objects fail closed; related independent Slice coverage: `SliceChunkTest.WindowCacheRejectsShortCommittedObjectWithoutPublication` (not a migrated COW test) |
| 71 | `RewriteHydrationRejectsDescriptorLargerThanChunk` | P | retired-private | — (not required) | COW descriptor length constraints retired; Slice corrupted record history fails closed; related independent Slice coverage: `SliceChunkTest.CorruptPublishedHistoryAndFailedSnapshotAreHardErrors` (not a migrated COW test) |
| 72 | `RewriteHydratesZeroLengthPublishedChunkWithoutBackendRead` | P | retired-private | — (not required) | COW empty-head hydration retired; Slice empty writes do not create dirty state; related independent Slice coverage: `SliceChunkTest.EmptyWriteDoesNotCreateDirtyStateAndBoundaryClampHidesPublishedTail` (not a migrated COW test) |
| 73 | `WriteAfterFailedInitialCommitPublishesLatestLocalData` | V | direct | `FileReadWriterSliceSizeTest.FailedFirstMetadataAttachmentPreservesLaterWrites` | FileMetadata first attachment returns EIO without committing; new overlapping/append writes survive retry; no COW revision-key assertions |
| 74 | `WriteAfterAmbiguousTypedAttachmentPublishesLatestLocalData` | V | adapted | `FileReadWriterSliceSizeTest.WritesDuringAmbiguousFirstAttachmentPreserveLatestGeneration` | Common accepted-write ordering after ambiguous confirmed first attachment; COW private head identity, retry strategy and exact timing are not transferred |
| 75 | `WriteAfterFailedRewriteCommitPublishesLatestLocalData` | P | retired-private | — (not required) | COW rewrite-revision retry retired; Slice finalization retry remains idempotent; related independent Slice coverage: `SliceChunkTest.FailedFileMetadataFinalizeRetriesWithoutDuplicateSliceAppend` (not a migrated COW test) |
| 76 | `WriteAfterAmbiguousRewriteCommitPublishesLatestLocalData` | P | retired-private | — (not required) | COW rewrite-head ambiguity retired; Slice ambiguous inode finalization retains records; related independent Slice coverage: `SliceChunkTest.LostAttachedInodeFinalizeReplyPreservesRecordForSafeRetry` (not a migrated COW test) |
| 77 | `RewriteRetryPropagatesMetadataLookupFailure` | P | retired-private | — (not required) | COW rewrite metadata lookup retired; FileReadWriter metadata read failures still propagate; related independent Slice coverage: `FileReadWriterSliceContractTest.MetadataLookupFailurePropagatesWithoutInventingSparseData` (not a migrated COW test) |
| 78 | `TypedRewriteRetryPreservesConcurrentFileEofGrowth` | P | retired-private | — (not required) | COW head CAS vs EOF race retired; shared FileReadWriter generation ordering remains tested; related independent Slice coverage: `FileReadWriterSliceSizeTest.ConcurrentFlushesPublishGenerationsInOrder` (not a migrated COW test) |
| 79 | `DetachedTypedAttachmentCannotBeReusedByStaleDirtySession` | V | adapted | `SliceChunkTest.BoundaryClampDoesNotMaskReplacedAuthoritativeChunkId` | Related Slice safety: replaced FileMetadata attachment is authoritative; stale COW dirty-head rejection/EEXIST and private GC semantics are retired |
| 80 | `TypedRewriteRetryAfterFailedPutDoesNotDeleteUnuploadedObject` | P | retired-private | — (not required) | COW deletion of attempted revisions retired; Slice failed puts never reclaim live dirty state; related independent Slice coverage: `SliceChunkTest.FailedDefinitePutUsesFreshKeyOnRetryWithoutRemovingLocalDirtyData` (not a migrated COW test) |
| 81 | `TypedRewriteFinalizationNotFoundNeverDeletesPublishedPrivateHead` | P | retired-private | — (not required) | COW head-GC policy retired; Slice attachment changes cannot be hidden by local cache; related independent Slice coverage: `SliceChunkTest.BoundaryClampDoesNotMaskReplacedAuthoritativeChunkId` (not a migrated COW test) |
| 82 | `TypedRewriteAfterFailedFinalizationRejectsNewerPrivateHead` | P | retired-private | — (not required) | COW newer head CAS retired; Slice replacement ChunkID and provenance remains authoritative; related independent Slice coverage: `SliceChunkTest.BoundaryClampDoesNotMaskReplacedAuthoritativeChunkId` (not a migrated COW test) |
| 83 | `TypedRewriteRemainsRetryableAfterTransientMetadataSnapshotFailure` | V | adapted | `FileReadWriterSliceSizeTest.FailedFirstMetadataAttachmentPreservesLaterWrites` | Shared failure-and-retry safety after transient metadata fault is exercised independently; COW rewrite-head snapshot and retry sequence is retired |
| 84 | `AmbiguousCommittedTypedTruncateReconcilesLocalEofAndKeepsDirtyPrefix` | B | direct | `FileReadWriterSliceSizeTest.AmbiguousCommittedShrinkKeepsDirtyPrefixAndAuthoritativeEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 85 | `KnownPartialExecCommittedShrinkNeverServesOrRepublishesDetachedData` | B | direct | `FileReadWriterSliceSizeTest.KnownPartialExecCommittedShrinkNeverRepublishesDetachedChunk` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 86 | `UnverifiablePartialExecSizeCommitPoisonsLiveSession` | B | direct | `FileReadWriterSliceSizeTest.UnverifiablePartialExecPoisonsAllLiveFileOperations` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 87 | `PartialExecWithPersistedEofAndUndetachedMappingFailsClosed` | B | direct | `FileReadWriterSliceSizeTest.PartialExecEofWithUndetachedChunkFailsClosed` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 88 | `AmbiguousCommittedTypedSizeSetAttrReconcilesLocalEofAndKeepsDirtyPrefix` | B | direct | `FileReadWriterSliceSizeTest.AmbiguousCommittedSizeSetAttrKeepsDirtyPrefixAndAuthoritativeEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 89 | `PartialTruncateOfDirtyRewriteClampsExpectedDescriptor` | P | retired-private | — (not required) | COW descriptor truncation retired; Slice immutable published suffix never reappears; related independent Slice coverage: `FileReadWriterSliceSizeTest.TruncateAndGrowNeverReviveDiscardedSliceSuffix` (not a migrated COW test) |
| 90 | `TruncateAfterAmbiguousInitialPublicationMayLeaveUploadedObject` | P | retired-private | — (not required) | COW failed publish object cleanup retired; ambiguous Slice attach does not retire unconfirmed bytes; related independent Slice coverage: `SliceChunkTest.UnknownFirstIndexCreationAndAttachmentAreConfirmedBeforeRetirement` (not a migrated COW test) |
| 91 | `TruncateCommitsTypedFileSizeWithoutInventingMappings` | V | adapted | `FileReadWriterSliceSizeTest.GrowingEmptyFileDoesNotInventChunkMappings` | Whole-file Slice truncate changes EOF without inventing mappings |
| 92 | `RetainedTypedChunkCanFlushAfterShrinkClampsItsPrivateHead` | P | retired-private | — (not required) | COW private head clamp retired; Slice ordered records preserve truncate/grow zero cover; related independent Slice coverage: `SliceChunkTest.OldTruncatedSuffixStaysHiddenUntilCoveredByNewCommit` (not a migrated COW test) |
| 93 | `RejectedGrowCannotSanitizeAttachedHiddenHead` | P | retired-private | — (not required) | COW head CAS rollback retired; Slice boundary provenance failure must be fail closed; related independent Slice coverage: `SliceChunkTest.BoundaryClampDoesNotMaskReplacedAuthoritativeChunkId` (not a migrated COW test) |
| 94 | `TypedGrowRefreshesCachedHeadAfterSanitizingHiddenTail` | P | retired-private | — (not required) | COW cached head refresh retired; Slice boundary clamp observes later committed records; related independent Slice coverage: `SliceChunkTest.BoundaryClampTracksLaterCommittedHistoryForSameChunkId` (not a migrated COW test) |
| 95 | `UnchangedTruncateSanitizesAlreadyCommittedBoundaryAfterFailedAttempt` | P | retired-private | — (not required) | COW head sanitation retry retired; Slice repeated clamps must not resurrect hidden suffix; related independent Slice coverage: `SliceChunkTest.RepeatedClampKeepsNewlyPublishedPrefixAndNeverRevivesOldTail` (not a migrated COW test) |
| 96 | `UnchangedTruncateDoesNotAcknowledgeMissingAttachedPrivateHead` | P | retired-private | — (not required) | COW missing head sanitation retired; Slice missing index while mapped fails closed; related independent Slice coverage: `SliceChunkTest.BoundarySanitationRejectsMissingAttachedIndexAndPropagatesAppendError` (not a migrated COW test) |
| 97 | `TruncateBeyondSupportedFileSizeIsRejectedBeforeMetadataMutation` | V | direct | `FileReadWriterSliceSizeTest.InvalidSizeMutationsNeverEnterMetadataCommit` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 98 | `SetAttrBeyondSupportedFileSizeIsRejectedBeforeMetadataMutation` | V | direct | `FileReadWriterSliceSizeTest.InvalidSizeMutationsNeverEnterMetadataCommit` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 99 | `TruncatePropagatesMetaError` | E | direct | `FileReadWriterSliceSizeTest.FailedTruncateDoesNotCommitNewEof` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 100 | `TypedSizeTransitionPropagatesMappingSnapshotFailure` | V | direct | `FileReadWriterSliceSizeTest.FailedMappingSnapshotStopsTruncateBeforeMutation` | Truncate must propagate the mapping snapshot failure and perform no metadata mutation |
| 101 | `UnchangedTypedSizeStillPropagatesAttributeMutationError` | V | direct | `FileReadWriterSliceSizeTest.NoopSizeMutationStillUpdatesMtimeWithoutLegacySizeSetAttr` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 102 | `UnchangedTruncateNeverCallsLegacySizeSetAttr` | V | direct | `FileReadWriterSliceSizeTest.NoopSizeMutationStillUpdatesMtimeWithoutLegacySizeSetAttr` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 103 | `UnflushedWriteTruncatedToPersistedEofStillUpdatesMtime` | V | direct | `FileReadWriterSliceSizeTest.NoopSizeMutationStillUpdatesMtimeWithoutLegacySizeSetAttr` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 104 | `UnchangedTypedSizeRespectsExplicitMtime` | V | direct | `FileReadWriterSliceSizeTest.NoopSizeMutationPreservesExplicitMtimeSelection` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 105 | `UnchangedTypedSizePreservesExplicitMtimeNow` | V | direct | `FileReadWriterSliceSizeTest.NoopSizeMutationPreservesExplicitMtimeSelection` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 106 | `TypedSizePlanningRejectsCorruptedPriorEofWithoutMutatingMetadata` | V | direct | `FileReadWriterSliceSizeTest.CorruptedPriorEofCannotEnterTruncatePlanningOrCommit` | Corrupted old EOF must be rejected during size planning before metadata mutation |
| 107 | `TypedGrowRejectsMissingStillAttachedBoundaryHeadBeforeCommit` | P | retired-private | — (not required) | COW missing head pre-grow retired; Slice mapped index missing must fail closed; related independent Slice coverage: `SliceChunkTest.BoundarySanitationRejectsMissingAttachedIndexAndPropagatesAppendError` (not a migrated COW test) |
| 108 | `TypedShrinkKeepsCommittedEofWhenPrivateBoundaryHeadIsMissing` | P | retired-private | — (not required) | COW missing head post-shrink retired; Slice mapped index missing must fail closed; related independent Slice coverage: `SliceChunkTest.BoundarySanitationRejectsMissingAttachedIndexAndPropagatesAppendError` (not a migrated COW test) |
| 109 | `TruncateDropsDirtyChunks` | E | direct | `FileReadWriterSliceSizeTest.TruncateAndGrowNeverReviveDiscardedSliceSuffix` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 110 | `HighIndexCachedTruncateDoesNotAliasLowChunk` | V | direct | `FileReadWriterSliceSizeTest.HighIndexCachedTruncateNeverAliasesLowIndex` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 111 | `TypedTruncateDetachesWholeMappingsWithoutForegroundDelete` | V | adapted | `VfsLastLinkCleanupTest.TruncateDetachesMappedSlicesWithoutPhysicalDeletion` | Real Redis Slice truncate detaches durable mapped content but performs no physical data-engine Delete |
| 112 | `ConcurrentReadsProceedWhileAnotherReadWaitsForBackend` | E | direct | `FileReadWriterSliceSizeTest.ConcurrentReadsRemainIndependentWhenFirstBackendGetIsBlocked` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 113 | `IndependentChunkOverwriteHydrationDoesNotSerializeOnInode` | P | retired-private | — (not required) | COW overwrite hydration synchronization retired; Slice window cache hydration tested independently; related independent Slice coverage: `SliceChunkTest.WindowCacheHydratesCommittedBytesBetweenSmallWrites` (not a migrated COW test) |
| 114 | `WriteDuringBlockedFlushDoesNotWaitForRemotePut` | E | direct | `FileReadWriterSliceSizeTest.ForegroundWriteProgressesDuringBlockedFlushUpload` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 115 | `WriteDuringBlockedCommitDoesNotWaitForRemotePublication` | E | direct | `FileReadWriterSliceSizeTest.ForegroundWriteProgressesDuringBlockedMetadataCommit` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 116 | `FailedOlderPutPreservesRepeatedWritesDuringFlush` | E | direct | `FileReadWriterSliceSizeTest.FailedOlderUploadPreservesWritesAcceptedDuringFlush` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 117 | `WriteDuringAmbiguousTypedAttachmentPreservesLatestLocalGeneration` | V | direct | `FileReadWriterSliceSizeTest.WritesDuringAmbiguousFirstAttachmentPreserveLatestGeneration` | During a blocked, committed-but-ambiguous Slice first attachment, a newer write is accepted and survives both Flushes |
| 118 | `ConcurrentFlushBarriersPublishGenerationsInOrder` | E | direct | `FileReadWriterSliceSizeTest.ConcurrentFlushesPublishGenerationsInOrder` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 119 | `MultiChunkFlushStartsAnotherPutWhileFirstPutIsBlocked` | E | direct | `FileReadWriterSliceTest.ParallelChunkUploadMustNotStarveFileMetadataPublication` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 120 | `TruncateWaitsForBlockedFlushPublication` | E | direct | `FileReadWriterSliceSizeTest.TruncateCannotOvertakeBlockedFlushPublication` | Same public/typed-boundary invariant; no COW-private assertions retained |
| 121 | `TruncateWaitsForBlockedReadWithoutBlockingEventBase` | E | direct | `FileReadWriterSliceSizeTest.TruncateWaitsForReadWithoutBlockingEventLoop` | Same public/typed-boundary invariant; no COW-private assertions retained |

## Cross-layer VFS last-link coverage

- `VfsLastLinkCleanupTest.UnlinkDefersOrphanPreparationWhileDescriptorIsOpen` persists bytes, then protects open-handle fence and durable pending work; a guarded data engine asserts zero physical Deletes.
- `VfsLastLinkCleanupTest.RenameOverwriteDefersVictimReclaimUntilLastClose` protects a persisted rename victim and source identity and asserts zero physical Deletes.
- `VfsLastLinkCleanupTest.UnlinkOfNonfinalHardlinkDoesNotPublishOrphanWork` protects surviving hard-link references.
- `VfsLastLinkCleanupTest.TmpfileIsAnonymousUsableAndReclaimedAfterFinalClose` and
  `TmpfileLinkRevivesOrphanAndSurvivesOriginalClose` preserve anonymous inode lifecycle.
- `VfsLastLinkCleanupTest.TruncateDetachesMappedSlicesWithoutPhysicalDeletion` exercises real Redis Slice mapping detachment while asserting the guarded data engine has not invoked `Delete`.
- Physical S3 `Delete` success/failure/retry expectations from the seven former COW GC tests are
  deliberately retired: Slice deletion is *unsafe and disabled* until #273; Redis
  `PendingDelete` and `ReclaimWork` durability remain separately covered.

## Other removed COW test suites — no mechanical test transplantation

The 121-case matrix only inventories the historical `FileReadWriterTest` COW
fixture. The same ownership rule was applied to **all 81 other deleted COW
unit tests**, not just those 121. The removed fixture/support types are not
tests and do not require equivalent Slice mocks.

| Deleted COW suite | Historical tests | COW-only behavior intentionally retired | Existing independent current-design safety coverage |
|---|---:|---|---|
| `chunk/cow/COWChunkBoundaryTest.cpp` | 10 | Private-head size/revision, head CAS conflict/retry, and COW-specific mapping probes | Slice boundary sanitation and mapped-index corruption handling in `SliceChunkTest` |
| `chunk/cow/COWChunkTest.cpp` | 32 | Concrete COWChunk, private-head hydration/CAS, revision allocation and publication format | Generic offset/range, write/read/flush, concurrent generations and typed factory failure contracts in `SliceChunkTest`, `FileReadWriterSliceContractTest` and `FileReadWriterSliceSizeTest` |
| `chunk/cow/COWObjectKeyTest.cpp` | 1 | Revision-derived immutable COW key encoding | No replacement required; Slice has an independent `SliceObjectKey` and its own object-identity safety tests |
| `chunk/cow/WriteBufTest.cpp` | 26 | The removed COW-specific WriteBuf data structure, threshold, clone/copy, and private buffer mechanics | Public dirty-read, sparse gap, overwrite, truncate/grow and frozen-write consistency contracts in Slice/FileReadWriter tests; no duplicate WriteBuf implementation or API |
| `metadata/redis/RedisChunkMetadataTest.cpp` | 12 | Redis COW typed-head serialization, revision counters/CAS and legacy per-COW-key layout | Current volume-scoped ChunkID monotonicity, ambiguous allocation, corrupt/negative counter and Slice record failure contracts in `RedisSliceChunkMetadataTest` |

**Coverage decision, not numeric parity:** 10 + 32 + 1 + 26 + 12 = 81
removed COW unit tests, beyond the 121 FileReadWriter fixture cases (202
historical test declarations inspected). Some deleted COW suites mix portable
checks with private algorithm expectations; portable checks are exercised by
the surviving mechanism-neutral/independent Slice test suites listed above.
No Slice test is created merely to reproduce a COW buffer threshold, revision
count, COW head CAS error, COW object-key byte format, or retired physical GC.
The bounded mounted COW E2E smoke was also removed; primary Slice E2E remains
and explicit retired-COW format/mount tests fail closed.

## Verification

This audit documents the surviving source-level witnesses; executable acceptance still requires
PR #529 Debug/Release native UT, E2E, pjdfstest, fstests and static/coverage CI green.
Do not interpret this mapping as a waiver of #525 main-CI root-cause acceptance.
