# Volume, CLI and E2E test independence from Memory Metadata (#441)

## Contract

After Memory Metadata is removed, Volume composition, CLI parsing, persistent
volume loading, and E2E test configuration must retain their observable
behavior. #441 removes test dependencies; #443 owns production Memory deletion,
registration/validator/help updates, and the final `memory://local` rejection.
No on-disk compatibility or substitute memory namespace is required in beta.

## Authority-specific test fixtures

- **Offline composition/error tests:** use the existing test-only
  `swordfs-test-meta` registry, `ConfiguredMetaEngine` and
  `UnsupportedMetaEngine` with narrowly tailored overrides. This preserves
  error propagation and typed chunk capability tests without network I/O.
  A configured fake `LoadVolume` is not evidence of persistent metadata.
- **Actual Redis format/load/mount tests:** allocate unique per-test volume
  names and call `VolumeImpl::CreateFrom` and `VolumeImpl::LoadFrom` against
  `SWORDFS_REDIS_TEST_URL`. Redis owns persisted volume identity and its
  format record. Re-format with the same name must return AlreadyExists;
  load of a missing name must return NotFound. A separate `VolumeImpl`
  instance models remount; where storage is needed, test data-engine factory
  injection uses the already registered `swordfs-test-data` engine.
- **CLI-only parsing:** use syntactically valid `redis://localhost:6379`
  values; validators do not establish a Redis connection. Unsupported schemes
  and malformed Redis parameters remain independently asserted. Positive
  Memory-specific validator cases stop being normative in #441, while the
  explicit negative regression assertion starts in #443 after its removal.
- **E2E:** `Fixture::GetLimits()` rejects missing or empty
  `SWORDFS_METADATA_URL` just like `FormatVolume()` and `StartMount()`, never
  silently substitutes Memory. `scripts/testing/run-e2e.sh` already supplies
  the Redis URL.

## Test disposition: local Memory VolumeFile

The nine `VolumeFileTest` cases in `VolumeFormatTest.cpp` test only the
Memory backend's `/etc/swordfs/<volume>/volume.fmt` implementation:

- `WriteAndReadRoundTrip`: retire local file storage; preserve shared
  `SwordFsVolume::SerializeTo/ParseFrom` roundtrip and Redis format/load.
- `WriteCreatesParentDir`, `WriteReusesExistingVolumeDirectory`,
  `WriteRejectsNonDirectoryVolumePath`,
  `WriteReportsVolumeDirectoryLookupFailure`,
  `WriteReportsConfigFileWriteFailure`: retire private filesystem path,
  directory creation, and OS-error handling with the Memory backend.
- `ReadNotFound`, `Exists`, `ReadRejectsNullOutput`: retire private file
  API; retain Redis missing-volume behavior and backend-neutral volume
  deserialize error checks. No new file-based test store is introduced.

All seven backend-neutral `SwordFsVolumeTest` declarations remain. The
VolumeImpl, CLI, and Validator suites must retain meaningful positive/error
scenarios, not just declaration names. The #439 D-class fixture inventory is
the reference for the final #443 audit.

## Verification and responsibilities

Check for no `MemMetaImpl`, `VolumeFile`, or `memory://local` test dependency
in the #441-scoped tests/E2E; do not remove `src/metadata/mem` here. Run
GitHub Debug/Release UT (Redis service started by runner), Redis/MinIO E2E,
pjdfstest and fstests; report skipped tests as incomplete coverage. If test
failures identify missing expected semantics, repair the narrow fixture or
assertion without introducing new production-only test APIs.

## Per-test semantic disposition

The #441 migration retains 25 `VolumeImplTest.cpp` declarations (the first
five config-adapter tests, one execution-domain death test, and 19 Volume
lifecycle/error tests); GTest fixture names may change from `VolumeImplTest`
to `VolumeImplRedisTest` when a real Redis service is needed. Here is the
assertion-level routing for those 19 tests:

| Original VolumeImpl test | Authority after migration | Preserved contract |
| --- | --- | --- |
| `CreateFromSucceeds` | Offline registry | Format composes typed COW capability; returns success and correct config |
| `CreateFromRejectsInvalidBucketUrl` | Offline validation | Invalid bucket syntax returns `EINVAL` |
| `CreateFromRejectsInvalidMetadataUrl` | Offline registry parsing | Malformed engine URL returns `EINVAL` |
| `FormatRejectsUnimplementedChunkType` | Redis (negative format/load) | Unsupported chunk mechanism returns `ENOSYS`; Redis volume remains absent (`ENOENT` on Load), preserving the former no-file-side-effect assertion |
| `MountUsesPersistedChunkTypeAndRejectsUnimplementedType` | Redis plus offline injected invalid type | Real Format/Load preserves COW type; unsupported persisted mechanism returns `ENOSYS` |
| `MountRejectsInvalidChunkMetadataCapability` | Offline typed fake | Null and wrong-type chunk capabilities fail closed |
| `MountRejectsCleanupMetadataClassMismatch` | Offline typed fake + test data engine | Wrong COW implementation cannot compose cleanup |
| `CreateFromNormalizesDataEngineIdentity` | Offline registry | Uppercase S3 URL maps to canonical `s3` engine identity |
| `CreateFromRedisEngine` | Redis | Real format/load preserves name, bucket, engine, region, type; duplicate format rejects |
| `LoadFromS3Engine` | Redis, real S3 engine initialization | Mount reloads persisted S3 bucket and region |
| `LoadFromUnknownDataEngine` | Offline injected config | Unregistered engine returns `ENOSYS` |
| `LoadFromUsesPersistedDataEngineIdentity` | Offline injected config | Engine comes from persisted `storage`, not bucket URL scheme |
| `LoadFromRejectsMissingDataEngineIdentity` | Offline production record decoder | One-sided missing storage identity fails as malformed `EIO` |
| `LoadFromRejectsMissingDataEngineLocation` | Offline production record decoder | One-sided missing storage location fails as malformed `EIO` |
| `LoadFromS3UrlMissingBucketName` | Offline injected config, S3 factory | Storage URL error still names missing bucket |
| `CreateFromVolumeAlreadyExists` | Redis | Second actual `VolumeImpl::CreateFrom` fails `EEXIST` |
| `LoadFromSucceeds` | Redis | Redis persisted format roundtrips via new `VolumeImpl` and composes COW runtime |
| `LoadFromUnsupportedEngine` | Offline registry | Unknown engine scheme returns `ENOSYS` (instead of relying on an unformatted Redis volume) |
| `LoadFromMissingFile` | Redis | Missing Redis formatted volume returns `ENOENT` |

The five adapter tests still assert CLI-to-volume projection, error handling,
ACL enablement, selected subcommand and independent storage thread count;
the debug execution-domain test remains untouched. Eighteen ConfigCenter
parameter tests retain their argument, validation and forwarding checks,
changing only the harmless metadata URL input. The six original metadata
validator tests remain six, with the former two Memory-specific tests now
checking valid Redis database selection and malformed Redis database names.
No parser test gains a running Redis dependency.
