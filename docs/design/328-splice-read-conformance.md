# generic/591 splice-read conformance investigation

Issue: [#328](https://github.com/SwordInfra/SwordFS/issues/328)

## Decision

`generic/591` is not evidence of a SwordFS splice semantic defect. The pinned
upstream helper has a concurrent pipe-reader bug that can close the read end
after a short `read(2)`, while the producer still has bytes to splice. SwordFS
must not change its data path merely to make that scheduling-sensitive helper
pass.

The conformance baseline therefore classifies the current pinned
`generic/591` failure as `upstream_test_defect`, not
`known_semantic_defect`. The tested application-level splice contract remains
applicable to SwordFS and is covered independently by a robust four-mode E2E
test until upstream provides a corrected testcase.

The separate `FUSE_CAP_SPLICE_READ` advertisement mismatch discovered during
this investigation is tracked by #356. It is deliberately not treated as the
cause of `generic/591`.

## Observed failure

On `main`, fstests run `36246336966` records `generic/591` as a real raw FAIL
in the `baseline-rest-1` shard. Only the first mode emits an extra line:

```text
concurrent reader with O_DIRECT
splice-test: splice: Broken pipe
concurrent reader without O_DIRECT
sequential reader with O_DIRECT
sequential reader without O_DIRECT
```

The top-level fstests gate is still green because the old baseline admitted
this exact result as a known semantic defect. That gate status is not evidence
that the testcase itself passed.

## Upstream helper root cause

Pinned fstests commit `a370dcbed43563f0462801e889e0eceb93c7cfad` runs
`src/splice-test` for all four `generic/591` modes. Its pipe reader loops over
`read(2)`, but reduces its remaining byte count by the requested read size
instead of the number of bytes actually returned:

```c
ret = read(fd, buffer, sz);
...
size -= sz;
```

A pipe read is allowed to return fewer than `sz` bytes. In the concurrent
mode, such a short read makes the child believe it consumed the entire
remaining payload and exit. Closing the pipe read end while the parent is
still in the splice loop produces SIGPIPE/EPIPE, which is reported as the
observed `Broken pipe`.

This is not FUSE-specific. Building the same pinned helper and running
`splice-test -s 4096 -r` against both a native ext4 file and a tmpfs file on
the development host reproduced exit status 141 (SIGPIPE) without SwordFS in
the path. The current upstream fstests source still contains the same
`size -= sz` logic, so changing only the pinned revision would not remove the
defect today.

As a controlled A/B check, changing only that decrement to `size -= ret` in a
temporary copy of the pinned helper makes all four `generic/591` modes pass on
both native ext4 and tmpfs. No SwordFS or FUSE behavior is involved in either
side of that control experiment. This isolates the helper's short-read
accounting as the cause of the SIGPIPE/EPIPE failure rather than merely a
correlated timing difference.

## Why O_DIRECT exposes the race in this run

The pinned testcase chooses the direct-I/O payload as 150 times the value from
`min_dio_alignment`. For a non-block FUSE mount without `STATX_DIOALIGN`, that
helper falls back to the page size. The non-O_DIRECT mode keeps its 512-byte
default sector size. As a result, the direct mode commonly exercises a much
larger transfer and exposes the buggy concurrent reader more readily.

Linux FUSE also has a meaningful cache-path difference here:

- userspace `O_DIRECT` is passed in the FUSE OPEN request;
- SwordFS does not return `FOPEN_DIRECT_IO` for ordinary files;
- an `O_DIRECT` write therefore uses the kernel's FUSE direct-I/O path and
  does not populate the file page cache;
- application `splice(file -> pipe)` uses the FUSE file operation
  `fuse_splice_read`, which in Linux 6.17 calls `filemap_splice_read` for a
  normal FUSE file.

That page-cache state affects the scheduling and chunking seen by the buggy
helper, but it does not make a short pipe read an invalid filesystem result.
The kernel documentation for `filemap_splice_read` explicitly permits short
reads when pipe space is insufficient.

## `FUSE_CAP_SPLICE_READ` is a different surface

libfuse documents `FUSE_CAP_SPLICE_READ` as permission for libfuse to use
`splice(2)` while **reading requests from `/dev/fuse`**, primarily so a
filesystem implementing the low-level `write_buf()` callback can receive
write data through a pipe. It does not advertise application-visible
`splice(file -> pipe)` support.

SwordFS currently requests this capability while its low-level operation table
has `.write_buf = nullptr`. That configuration should be made internally
consistent, but toggling it is not a justified fix for `generic/591`; #356
owns that cleanup.

## Regression contract

`tests/e2e/SpliceReadTest.cpp` covers the application-visible contract without
the upstream reader bug. It exercises the full matrix:

| Reader | File mode |
| --- | --- |
| concurrent | `O_DIRECT` |
| concurrent | buffered |
| sequential | `O_DIRECT` |
| sequential | buffered |

Every mode writes the same page-aligned payload, splices until the complete
payload has been transferred, drains the pipe by subtracting the actual
`read(2)` return value, and validates byte contents. This intentionally tests
filesystem semantics rather than a particular pipe scheduling pattern.

## Baseline policy

`upstream_test_defect` is reserved for a failing upstream testcase whose
failure mechanism is independently reproduced without SwordFS or otherwise
proven to be invalid in the upstream test itself. It is not a substitute for
`known_semantic_defect`, `known_unsupported`, or `environment`.

If the pinned testcase is corrected and begins passing, the strict classifier
will report XPASS. At that point `generic/591` must be re-evaluated and moved
to `supported.txt` only when the corrected upstream test provides valid
conformance evidence.
