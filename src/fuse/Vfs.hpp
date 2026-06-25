// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS
// VFS.
//
// Uses the libfuse low-level API (fuse_lowlevel_ops) which operates at the
// inode level rather than the path level.

#pragma once

#define FUSE_USE_VERSION 31
#include <fuse_lowlevel.h>

namespace swordfs::fuse {

/// The FUSE low-level operation table exposed so callers can build sessions.
extern const struct fuse_lowlevel_ops swordfs_ll_ops;

}  // namespace swordfs::fuse
