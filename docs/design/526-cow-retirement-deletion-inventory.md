# COW FileReadWriter deletion inventory — 121 cases

This inventory is the explicit Stage A / Stage B per-test gate. Classifications are **review dispositions**, not proof of equivalent coverage. Re-evaluate against the final code and record an exact surviving test before deleting any generic case.

| Code | Meaning | Release gate |
| --- | --- | --- |
| B | Stage A priority common contract | Must have an independent Slice test in #528 |
| V | Generic or mixed; independent coverage still unverified | Stage B must name equivalent surviving test, or add one before deleting |
| E | Existing independent Slice / VFS / Redis candidate | Stage B must verify comparable behavioral assertion by named test |
| P | Dominantly concrete COW revision/head/key/hydration contract | Stage B may retire the private assertion; inspect for common sub-contracts first |

**Stage A priority case mapping (requires current-head CI and reviewer confirmation):**

| COW test | Independent Slice test |
| --- | --- |
| `HighOffsetWriteFlushAndFreshReadPreserveLogicalIndex` | `FileReadWriterSliceSizeTest.HighOffsetWriteFlushAndColdReadPreserve64BitChunkIndex` |
| `AmbiguousCommittedTypedTruncateReconcilesLocalEofAndKeepsDirtyPrefix` | `FileReadWriterSliceSizeTest.AmbiguousCommittedShrinkKeepsDirtyPrefixAndAuthoritativeEof` |
| `KnownPartialExecCommittedShrinkNeverServesOrRepublishesDetachedData` | `FileReadWriterSliceSizeTest.KnownPartialExecCommittedShrinkNeverRepublishesDetachedChunk` |
| `UnverifiablePartialExecSizeCommitPoisonsLiveSession` | `FileReadWriterSliceSizeTest.UnverifiablePartialExecPoisonsAllLiveFileOperations` |
| `PartialExecWithPersistedEofAndUndetachedMappingFailsClosed` | `FileReadWriterSliceSizeTest.PartialExecEofWithUndetachedChunkFailsClosed` |
| `AmbiguousCommittedTypedSizeSetAttrReconcilesLocalEofAndKeepsDirtyPrefix` | `FileReadWriterSliceSizeTest.AmbiguousCommittedSizeSetAttrKeepsDirtyPrefixAndAuthoritativeEof` |

## Full COW fixture inventory

| # | COW fixture test (`FileReadWriterTest.*`) | Disposition |
| ---: | --- | :---: |
| 1 | `FullChunk` | **E** |
| 2 | `WithinChunkWithOffset` | **E** |
| 3 | `PartialChunkAtEOF` | **E** |
| 4 | `PersistedPartialChunkAtEOFReturnsShortRead` | **V** |
| 5 | `PastEOF` | **E** |
| 6 | `CrossChunkBoundary` | **E** |
| 7 | `CrossChunkExactBoundary` | **E** |
| 8 | `CrossChunkReadsIntoSecondChunk` | **E** |
| 9 | `CrossChunkExhaustsSecondChunk` | **V** |
| 10 | `CrossChunkSecondChunkMissing` | **E** |
| 11 | `ZeroSizeRequest` | **E** |
| 12 | `NegativeReadOffsetIsRejected` | **E** |
| 13 | `ZeroLengthNegativeReadOffsetIsRejected` | **E** |
| 14 | `NegativeWriteOffsetIsRejectedBeforeChunkLookup` | **E** |
| 15 | `HighOffsetWriteFlushAndFreshReadPreserveLogicalIndex` | **B** |
| 16 | `WritePastSupportedFileSizeIsRejectedBeforeMutation` | **V** |
| 17 | `VisibleSizeMetadataFailurePropagatesFromRead` | **V** |
| 18 | `ReadRejectsPersistedSizeBeyondSupportedFileRange` | **V** |
| 19 | `VisibleSizeSnapshotCannotRegressBehindCompletedFlush` | **E** |
| 20 | `EmptyOutputOnNoData` | **E** |
| 21 | `MissingChunkMetadataReadsAsSparseZeros` | **E** |
| 22 | `MaterializedPublishedChunkZeroFillsItsUncoveredTail` | **V** |
| 23 | `PersistedChunkShortReadFailsClosed` | **V** |
| 24 | `PersistedChunkReadErrorRollsBackPartialOutput` | **P** |
| 25 | `PersistedChunkZeroLengthReadIsNoOp` | **P** |
| 26 | `ChunkMetadataIOErrorPropagatesFromRead` | **E** |
| 27 | `MalformedChunkMetadataPropagatesFromRead` | **E** |
| 28 | `ChunkMetadataIOErrorPropagatesFromWrite` | **V** |
| 29 | `CrossChunkMetadataErrorDrainsSubmittedReadBeforeReturning` | **E** |
| 30 | `SparseReadWithMultipleHoles` | **E** |
| 31 | `FlushRetriesPutFailureUntilDataIsPublished` | **E** |
| 32 | `PutFailureRetryPublishesNewTypedAttachment` | **E** |
| 33 | `SuccessfulFirstFlushIsIdempotentWithoutAdditionalWrites` | **V** |
| 34 | `FlushRetriesRevisionAllocationFailureBeforeUploadingObject` | **P** |
| 35 | `CompetingInitialPublishersCannotReplaceWinningTypedAttachment` | **V** |
| 36 | `FlushRetriesPublicationFailureWithFreshRevision` | **P** |
| 37 | `AmbiguousUncommittedTypedAttachmentRetriesWithFreshChunkIdentity` | **V** |
| 38 | `DirtyUnattachedWriterRejectsCompetingTypedAttachment` | **V** |
| 39 | `RejectedTypedAttachmentRetryUsesFreshImmutableIdentity` | **V** |
| 40 | `FirstAttachmentUnknownSizeOnlyDoesNotReplayCandidate` | **V** |
| 41 | `FirstAttachmentUnknownMappingOnlyFailsClosed` | **V** |
| 42 | `FirstAttachmentKnownExecCommandFailureDoesNotRepairOrReplay` | **V** |
| 43 | `CachedCleanReadUsesLatestPrivateHeadAfterExternalRewrite` | **P** |
| 44 | `MissingCurrentPrivateObjectIsNotMistakenForGcRace` | **P** |
| 45 | `FirstAttachmentRetriesWhenEofBoundaryAttachesWithoutSizeChange` | **V** |
| 46 | `SuccessfulTypedRewritesQueueEverySupersededRevision` | **P** |
| 47 | `RejectedTypedRewriteRetriesWithFreshPrivateRevision` | **P** |
| 48 | `FlushNeverShrinksExistingFileSize` | **V** |
| 49 | `SuccessfulFlushClearsTransientLiveSize` | **V** |
| 50 | `GetAttrSnapshotCannotRegressReadEOFBehindCompletedFlush` | **E** |
| 51 | `LiveAttrGuardUsesTransientSize` | **V** |
| 52 | `GetAttrBlocksDoNotPublishCachedWrite` | **V** |
| 53 | `SparseCachedWriteKeepsBlocksBelowLogicalSize` | **V** |
| 54 | `FailedFlushKeepsTransientLiveSize` | **E** |
| 55 | `SuccessfulTruncateDoesNotLeaveTransientSizeOverlay` | **V** |
| 56 | `FlushContinuesOtherChunksAfterOneChunkFails` | **V** |
| 57 | `AmbiguousTypedFirstAttachmentReconcilesBeforeTruncate` | **V** |
| 58 | `WriteAfterPartialTruncateZeroFillsDiscardedRange` | **E** |
| 59 | `UnflushedWriteVisibleAcrossHandles` | **V** |
| 60 | `FlushedDataVisibleAcrossHandles` | **V** |
| 61 | `WriteAfterFlushOverwritesExistingChunkWithoutLosingUntouchedBytes` | **E** |
| 62 | `ReopenedWriterAppendsWithinExistingFlushedChunk` | **E** |
| 63 | `TypedRewriteAdvancesPrivateHeadAndRetainsImmutableObjectVersions` | **P** |
| 64 | `RewritePutFailureKeepsOldVersionAuthoritativeAndCanRetry` | **V** |
| 65 | `TypedRewriteAfterAmbiguousFinalizationPublishesFreshPrivateRevision` | **P** |
| 66 | `AmbiguousRewriteRetryFailureKeepsLatestDataWritable` | **P** |
| 67 | `TypedRewriteRetryFinalizesEofAfterSuccessfulPrivateHeadCas` | **P** |
| 68 | `TypedRewriteRejectsAnExternallyAdvancedPrivateHeadWithoutDeletingIt` | **P** |
| 69 | `RewriteHydrationPropagatesBackendReadFailure` | **P** |
| 70 | `RewriteHydrationRejectsObjectShorterThanDescriptor` | **P** |
| 71 | `RewriteHydrationRejectsDescriptorLargerThanChunk` | **P** |
| 72 | `RewriteHydratesZeroLengthPublishedChunkWithoutBackendRead` | **P** |
| 73 | `WriteAfterFailedInitialCommitPublishesLatestLocalData` | **V** |
| 74 | `WriteAfterAmbiguousTypedAttachmentPublishesLatestLocalData` | **V** |
| 75 | `WriteAfterFailedRewriteCommitPublishesLatestLocalData` | **P** |
| 76 | `WriteAfterAmbiguousRewriteCommitPublishesLatestLocalData` | **P** |
| 77 | `RewriteRetryPropagatesMetadataLookupFailure` | **P** |
| 78 | `TypedRewriteRetryPreservesConcurrentFileEofGrowth` | **P** |
| 79 | `DetachedTypedAttachmentCannotBeReusedByStaleDirtySession` | **V** |
| 80 | `TypedRewriteRetryAfterFailedPutDoesNotDeleteUnuploadedObject` | **P** |
| 81 | `TypedRewriteFinalizationNotFoundNeverDeletesPublishedPrivateHead` | **P** |
| 82 | `TypedRewriteAfterFailedFinalizationRejectsNewerPrivateHead` | **P** |
| 83 | `TypedRewriteRemainsRetryableAfterTransientMetadataSnapshotFailure` | **V** |
| 84 | `AmbiguousCommittedTypedTruncateReconcilesLocalEofAndKeepsDirtyPrefix` | **B** |
| 85 | `KnownPartialExecCommittedShrinkNeverServesOrRepublishesDetachedData` | **B** |
| 86 | `UnverifiablePartialExecSizeCommitPoisonsLiveSession` | **B** |
| 87 | `PartialExecWithPersistedEofAndUndetachedMappingFailsClosed` | **B** |
| 88 | `AmbiguousCommittedTypedSizeSetAttrReconcilesLocalEofAndKeepsDirtyPrefix` | **B** |
| 89 | `PartialTruncateOfDirtyRewriteClampsExpectedDescriptor` | **P** |
| 90 | `TruncateAfterAmbiguousInitialPublicationMayLeaveUploadedObject` | **P** |
| 91 | `TruncateCommitsTypedFileSizeWithoutInventingMappings` | **V** |
| 92 | `RetainedTypedChunkCanFlushAfterShrinkClampsItsPrivateHead` | **P** |
| 93 | `RejectedGrowCannotSanitizeAttachedHiddenHead` | **P** |
| 94 | `TypedGrowRefreshesCachedHeadAfterSanitizingHiddenTail` | **P** |
| 95 | `UnchangedTruncateSanitizesAlreadyCommittedBoundaryAfterFailedAttempt` | **P** |
| 96 | `UnchangedTruncateDoesNotAcknowledgeMissingAttachedPrivateHead` | **P** |
| 97 | `TruncateBeyondSupportedFileSizeIsRejectedBeforeMetadataMutation` | **V** |
| 98 | `SetAttrBeyondSupportedFileSizeIsRejectedBeforeMetadataMutation` | **V** |
| 99 | `TruncatePropagatesMetaError` | **E** |
| 100 | `TypedSizeTransitionPropagatesMappingSnapshotFailure` | **V** |
| 101 | `UnchangedTypedSizeStillPropagatesAttributeMutationError` | **V** |
| 102 | `UnchangedTruncateNeverCallsLegacySizeSetAttr` | **V** |
| 103 | `UnflushedWriteTruncatedToPersistedEofStillUpdatesMtime` | **V** |
| 104 | `UnchangedTypedSizeRespectsExplicitMtime` | **V** |
| 105 | `UnchangedTypedSizePreservesExplicitMtimeNow` | **V** |
| 106 | `TypedSizePlanningRejectsCorruptedPriorEofWithoutMutatingMetadata` | **V** |
| 107 | `TypedGrowRejectsMissingStillAttachedBoundaryHeadBeforeCommit` | **P** |
| 108 | `TypedShrinkKeepsCommittedEofWhenPrivateBoundaryHeadIsMissing` | **P** |
| 109 | `TruncateDropsDirtyChunks` | **E** |
| 110 | `HighIndexCachedTruncateDoesNotAliasLowChunk` | **V** |
| 111 | `TypedTruncateDetachesWholeMappingsWithoutForegroundDelete` | **V** |
| 112 | `ConcurrentReadsProceedWhileAnotherReadWaitsForBackend` | **E** |
| 113 | `IndependentChunkOverwriteHydrationDoesNotSerializeOnInode` | **P** |
| 114 | `WriteDuringBlockedFlushDoesNotWaitForRemotePut` | **E** |
| 115 | `WriteDuringBlockedCommitDoesNotWaitForRemotePublication` | **E** |
| 116 | `FailedOlderPutPreservesRepeatedWritesDuringFlush` | **E** |
| 117 | `WriteDuringAmbiguousTypedAttachmentPreservesLatestLocalGeneration` | **V** |
| 118 | `ConcurrentFlushBarriersPublishGenerationsInOrder` | **E** |
| 119 | `MultiChunkFlushStartsAnotherPutWhileFirstPutIsBlocked` | **E** |
| 120 | `TruncateWaitsForBlockedFlushPublication` | **E** |
| 121 | `TruncateWaitsForBlockedReadWithoutBlockingEventBase` | **E** |

## Stage B continuation checklist

- Confirm each **E** case has comparable explicit assertions in named surviving tests, not just the same broad topic.
- Resolve every **V** case to a named surviving test, newly migrated regression, or a design-approved COW-private-only retirement before deleting the original.
- Retire **P** only after examining whether shared error outcomes/inputs are covered elsewhere; COW-specific revision counts/object key bytes/CAS head identity have no Slice counterpart.
- Keep the existing FileReadWriter translation unit or extract its Slice fixtures before removing COW-only fixture, preserving compilation independent of COW headers.
- Confirm the six **B** new tests continue to run in Stage B without concrete COW dependencies.
- Retain generic RedisMetaImpl/Txn, Reclaimer, VolumeFormat, VFS and FileHandle tests; remove concrete COW-private byte-format/GC only.
- Keep ChunkType persistence identity stable and reject retired COW volumes fail-closed, without enabling Slice physical GC (#273 separate).
- Merge Stage A, pass main CI, then begin Stage B.
