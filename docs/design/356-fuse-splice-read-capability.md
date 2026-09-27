# FUSE splice-read capability alignment

## Context

SwordFS uses the libfuse low-level API. Its operation table currently leaves
`fuse_lowlevel_ops::write_buf` unset and receives write requests through the
ordinary `write` callback.

`FUSE_CAP_SPLICE_READ` does not describe application-visible
`splice(file -> pipe)` behavior. In libfuse it permits request payloads to be
spliced from `/dev/fuse` into a pipe for delivery to the low-level
`write_buf()` callback. libfuse therefore normally enables this capability
only when a filesystem implements `write_buf()`.

Before this change, `SwordFsInit` requested `FUSE_CAP_SPLICE_READ` while
`write_buf` remained null. That advertised a receive path the callback table
did not implement.

## Decision

SwordFS will explicitly disable `FUSE_CAP_SPLICE_READ` while `write_buf()` is
not implemented.

The init path must clear the capability even when libfuse or mount options
pre-populate `conn->want` / `conn->want_ext`. The low-level operation table
continues to use `.write` and leaves `.write_buf = nullptr`.

## Why not implement `write_buf()` here

Adding `write_buf()` would introduce a new request-buffer ownership and
lifetime path, including pipe-backed buffers and consumption-offset handling.
That is a data-path optimization rather than a prerequisite for preserving
SwordFS's current write semantics. It should be designed and measured
separately instead of expanding this capability-correctness fix.

This decision does not change application-visible read/write or
`splice(file -> pipe)` semantics, and it does not enable
`FUSE_CAP_SPLICE_WRITE`.

## Verification

A focused unit regression test covers both sides of the contract:

- init clears `FUSE_CAP_SPLICE_READ` from requested capability state even when
  the kernel advertises it; and
- the low-level operation table has no `write_buf` callback.

Full CI, static analysis, and per-file patch coverage remain merge gates.
