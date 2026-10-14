# FileHandle concurrent-test metadata reads (#478)

## Failure

The Release unit-test job on main commit `3176979` aborted in
`FileHandleTest.ConcurrentOpenAndFind` with glibc reporting
`double free or corruption (!prev)` (CI run `38527808026`). The test starts
four POSIX threads, each running a Folly fiber and opening independent
synthetic inodes through the same in-memory `MockMetaEngine`.

The mock's `Open()` accessed the shared `unordered_map` of inode EOF values
using `operator[]`. On a missing inode this performs an insertion, so the
parallel nominally read-only `Open()` calls raced on container mutation.
Concurrent insertions into an unprotected `unordered_map` are undefined
behavior and can corrupt its allocations.

## Contract and resolution

The mock's `Open()` must be read-only for previously unseen inodes; an
absent recorded EOF is logically zero. A `find()` lookup followed by a
zero default preserves the test behavior without mutating shared metadata.
It permits concurrent `Open()` calls when no other test code changes the
mock, without pretending the entire mutable mock is generally thread-safe.

The existing four-thread `ConcurrentOpenAndFind` test exercises this path.
The `HandleManager` and `InodeHandleManager` production registries already
serialize map mutation through fiber mutexes, and this test failure does not
establish a production lock or FUSE issue. No production API, locking
primitive, or compatibility behavior is changed.

## Verification

Verify a focused Release build and `FileHandleTest.ConcurrentOpenAndFind`
through manually dispatched GitHub Dev Build, followed by full PR CI and
the post-merge main CI. Existing PR #476 telemetry work is unrelated to
this mock-specific failure and issue #466 remains open.
