# Metadata backend contract and dependency boundary

## Purpose

`IMetaEngine` is the filesystem-facing Metadata contract. Redis is the current
production implementation, but VFS, FUSE, Volume and mechanism-neutral Chunk
logic must not depend on Redis schemas, key names, client types, transactions or
concrete engine classes. The contract remains independent of the current backend implementation.

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
- Backend-specific integration tests supplement rather than replace the portable
  contract; preserve meaningful edge conditions as current Redis regressions.

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
Do not add a new factory or parser abstraction merely to mirror this exception.

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
- Concurrent namespace tests must establish that their observers actually ran
  while validating each individual entry. Two separate lookups are not an
  atomic snapshot of a multi-entry namespace and must not be compared as one.
- Rejected ACL updates leave the published inode mode, ctime and ACL payload
  unchanged; checking an error status alone is not a rollback assertion.
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
SwordFS compilation is permitted by the engineering workflow. Review any
newly discovered gap against the active Redis contract.
