# #518 — Slice Chunk as the default for all new volumes

## Contract and boundaries

New volumes created through `swordfs format`, the programmatic `FormatOptions`
API, and `SwordFsVolume{}` must select `chunk_slice` when no mechanism is
specified. CLI `--chunk-type cow` and explicit `FormatOptions.chunk_type=kCow`
remain valid for independent COW regression. **Mount always trusts the already
persisted volume's `chunk_type`**, never the current format default; existing
COW and Slice volume encodings / Redis identifiers remain unchanged.

The common conformance helper and every default E2E/CI creation path use Slice
without requiring a Slice opt-in. COW tests must explicitly select COW instead
of accidentally depending on historical defaults. The unified #512 primary
Slice conformance gates form the rollout prerequisite; this separate #518 PR
must never reintroduce the duplicate full COW+Slice matrix.

## Mandatory pre-merge activation constraints

A default change is **not** a statement that general production usage is safe.
The roadmap's outstanding [#273 physical Slice GC](https://github.com/SwordInfra/SwordFS/issues/273),
[#274 compaction/history bounds](https://github.com/SwordInfra/SwordFS/issues/274),
[#202 dirty-memory/backpressure](https://github.com/SwordInfra/SwordFS/issues/202)
and [#517 residual EOF/attachment race](https://github.com/SwordInfra/SwordFS/issues/517)
require explicit resolution, or a separately recorded owner acceptance with
beta release limitations and safe deployment constraints. In particular, until
#273 is implemented, Slice detached objects cannot be reclaimed safely and
storage usage can grow without physical GC. Until #274 is implemented, ordered
Slice history can grow without a bounded compaction policy. These risks must
not be erased by switching a CLI default. #424's two owner-accepted beta data
integrity exceptions are **narrow** and do not waive #517's failed Flush.

Keep this implementation in Draft and require a new exact-head and main CI
validation before enabling the default for release. Reconcile with the #512
CI default branch first to avoid mixing COW expectation files and Slice test
outputs.

## Test-first verification

1. RED: GTest no-flag CLI `ConfigCenter.chunk_type()` is `chunk_slice`;
   default-constructed `FormatOptions` and `SwordFsVolume` select Slice;
   explicit COW preserves COW volume metadata.
2. GREEN: update the format defaults, script selection, focused tests and
   explicit COW-only fixtures. Verify COW persisted mount still selects COW,
   and unsupported `redis_cache` is still refused.
3. Validate full Debug/Release, mounted E2E, independent COW smoke, 8,770
   supported pjdfstest assertions, 10 pinned Slice fstests shards (145/145
   supported + 499 classified gaps), strict Flush/RELEASE log gate, review,
   static audit and Codecov. Production format default and CI must agree.
