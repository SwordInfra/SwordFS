// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS VFS.
//
// Contains fuse_operations callbacks. Modeled after JuiceFS pkg/fuse package.
//
// Current phase: minimal implementation — only the operations required for
// a successful mount. All I/O operations return -ENOSYS.

#pragma once

namespace swordfs::fuse {

/// Mount the filesystem via libfuse. Blocks until unmounted.
/// @return 0 on success, nonzero on failure.
int Mount(int argc, char* argv[]);

} // namespace swordfs::fuse
