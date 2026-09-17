# Chunk Publication Contract

SwordFS stores each flushed chunk as an immutable object and publishes a
descriptor for that object through the metadata engine. The object and its
metadata are intentionally separate durability domains, so their ordering is
the publication protocol.

## Invariants

1. A local dirty chunk is visible only to handles sharing the local
   `FileReadWriter`; it is not present in persistent chunk metadata.
2. `IDataEngine::Put()` returning `OK` means the complete immutable object is
   atomically readable under its revision-qualified key.
3. `IMetaEngine::CommitChunk()` is the reader-visibility barrier. A descriptor
   may be committed only after its object upload succeeds.
4. A bounded successful `Chunk::Read()` returns exactly the requested bytes.
   It validates the bytes appended by `IDataEngine::Get()`; short data is an
   I/O error, never a successful chunk read.

The resulting publication order is:

```text
local write buffer
       |
       v
allocate unique revision
       |
       v
Put complete immutable object
       |
       v
CommitChunk descriptor with CAS
       |
       v
readers may resolve the object
```

No `Writing` or `Uploading` record is stored in metadata. Readers therefore
have only two persistent states to interpret: no descriptor (a hole) or a
descriptor for a completed object.

## Failure and crash windows

| Failure point | Persistent result | Reader-visible result |
| --- | --- | --- |
| Before/during failed `Put` | No object is guaranteed; no metadata commit is attempted | The new chunk is not visible |
| After successful `Put`, before `CommitChunk` | A complete but unreachable object may remain | The new chunk is not visible |
| `CommitChunk` returns a definite conflict | The losing revision object is eligible for cleanup | The winning descriptor remains authoritative |
| `CommitChunk` result is ambiguous | Retry resolves the authoritative descriptor before another publication attempt | A committed matching descriptor can be completed idempotently |
| After successful `CommitChunk` | Complete object and authoritative descriptor exist | The chunk is readable |

Unreachable objects left by a process crash require persistent reconciliation
and garbage collection, tracked separately by GitHub issue #143. They do not
weaken the visibility invariant because no metadata descriptor references
them.

## Why there is no post-upload `HEAD`

The data engine's successful `Put()` result is the completion contract. S3
single-key PUTs are atomic and strongly read-after-write consistent; another
`HEAD` would add a storage round trip without strengthening atomicity. Backends
that cannot provide this contract must not return `OK` from `Put()` until they
have established equivalent semantics.

Read-side exact-length validation remains mandatory. It detects a backend that
violates the contract, external object corruption/truncation, and stale or
malformed metadata without exposing unwritten buffer capacity to callers.
