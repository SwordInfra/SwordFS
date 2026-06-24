// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS VFS.
//
// Uses the libfuse low-level API (fuse_lowlevel_ops) which operates at the
// inode level rather than the path level.
//
// Current phase: minimal implementation — only the operations required for
// a successful mount. All I/O operations return -ENOSYS.

#pragma once

namespace swordfs::fuse {

/// Mount the filesystem via libfuse low-level API. Blocks until unmounted.
/// @return 0 on success, nonzero on failure.
int Mount(int argc, char* argv[]);

}  // namespace swordfs::fuse
