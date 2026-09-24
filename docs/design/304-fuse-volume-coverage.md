# Issue #304: FUSE and Volume Coverage Floor

## Goal

Raise `src/fuse/Vfs.cpp` and `src/volume/VolumeImpl.cpp` from their `c39a82d`
baseline of 77.77% to at least 80% line coverage without adding test-only
production surfaces or line-execution-only tests.

## Coverage contract

Coverage is evidence that a production contract was exercised, not the reason
for inventing a contract. The tests in this slice therefore use existing
public boundaries and assert stable externally meaningful behavior.

For `Vfs.cpp`, the selected contracts are:

- entry-producing FUSE callbacks translate metadata failures into the matching
  kernel errno reply and do not publish a successful entry;
- `fsyncdir`, which is intentionally unsupported by `VfsImpl`, returns
  `ENOSYS` through the actual FUSE hook rather than only through the VFS method.

For `VolumeImpl.cpp`, the selected contracts are:

- malformed bucket URLs are rejected before storage identity is accepted;
- malformed metadata URLs are rejected before a metadata engine is created.

These cases cover currently missing adapter/error paths while remaining
resistant to internal refactoring.

## Non-goals

This slice does not add production hooks, registries, setters, or mocks for the
purpose of coverage. It also does not attempt to cover runtime-admission failure
or kernel capability combinations merely to increase the percentage; those
paths require separate behavioral justification if they remain relevant after
the 80% floor is met.

## Verification

The implementation is test-only. Local work may compile `swordfs_test`; FUSE
hook execution and the final coverage measurement remain GitHub-CI
authoritative under the SwordFS workflow. Codecov must show both target `.cpp`
files at or above 80% on the final PR head.
