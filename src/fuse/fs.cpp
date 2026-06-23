// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS VFS.
//
// Contains all fuse_operations callbacks. Similar to JuiceFS pkg/fuse package.
//
// Current phase: minimal implementation — only the operations required for
// a successful mount. All I/O operations currently return -ENOSYS.

#include "fuse/fs.hpp"

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <cstring>
#include <cstdlib>
#include <cerrno>

#include <folly/logging/xlog.h>

// ── FUSE filesystem operations ─────────────────────────────────────────

static void* SwordfsInit(struct fuse_conn_info* conn,
                          struct fuse_config* cfg) {
  (void)conn;

  // Disable kernel caching for now (no metadata backing store).
  cfg->kernel_cache     = 0;
  cfg->entry_timeout    = 1.0;
  cfg->attr_timeout     = 1.0;
  cfg->negative_timeout = 1.0;

  XLOG(INFO, "SwordFS filesystem initialized (mount OK)");
  return nullptr;
}

static void SwordfsDestroy(void* private_data) {
  (void)private_data;
  XLOG(INFO, "SwordFS filesystem unmounted");
}

static int SwordfsGetattr(const char* path, struct stat* stbuf,
                           struct fuse_file_info* fi) {
  (void)fi;
  std::memset(stbuf, 0, sizeof(struct stat));

  if (std::strcmp(path, "/") == 0) {
    stbuf->st_mode  = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    return 0;
  }

  return -ENOENT;
}

static int SwordfsReaddir(const char* path, void* buf,
                           fuse_fill_dir_t filler,
                           off_t offset,
                           struct fuse_file_info* fi,
                           enum fuse_readdir_flags flags) {
  (void)offset;
  (void)fi;
  (void)flags;

  if (std::strcmp(path, "/") != 0) {
    return -ENOENT;
  }

  filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_DEFAULTS);
  filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_DEFAULTS);

  return 0;
}

static int SwordfsStatfs(const char* path, struct statvfs* stbuf) {
  (void)path;

  std::memset(stbuf, 0, sizeof(struct statvfs));
  stbuf->f_namemax = 255;
  stbuf->f_frsize  = 4096;
  stbuf->f_bsize   = 4096;

  return 0;
}

static int SwordfsAccess(const char* path, int mask) {
  (void)path;
  (void)mask;
  return 0;
}

// ── Operation table (order matches struct fuse_operations declaration) ─

static const struct fuse_operations swordfs_ops = {
  .getattr   = SwordfsGetattr,
  .readlink  = nullptr,
  .mknod     = nullptr,
  .mkdir     = nullptr,
  .unlink    = nullptr,
  .rmdir     = nullptr,
  .symlink   = nullptr,
  .rename    = nullptr,
  .link      = nullptr,
  .chmod     = nullptr,
  .chown     = nullptr,
  .truncate  = nullptr,
  .open      = nullptr,
  .read      = nullptr,
  .write     = nullptr,
  .statfs    = SwordfsStatfs,
  .flush     = nullptr,
  .release   = nullptr,
  .fsync     = nullptr,
  .setxattr  = nullptr,
  .getxattr  = nullptr,
  .listxattr = nullptr,
  .removexattr = nullptr,
  .opendir   = nullptr,
  .readdir   = SwordfsReaddir,
  .releasedir = nullptr,
  .fsyncdir  = nullptr,
  .init      = SwordfsInit,
  .destroy   = SwordfsDestroy,
  .access    = SwordfsAccess,
  .create    = nullptr,
  .lock      = nullptr,
  .utimens   = nullptr,
  .bmap      = nullptr,
  .ioctl     = nullptr,
  .poll      = nullptr,
  .write_buf = nullptr,
  .read_buf  = nullptr,
  .flock     = nullptr,
  .fallocate = nullptr,
  .copy_file_range = nullptr,
  .lseek     = nullptr,
};

// ── Public API ─────────────────────────────────────────────────────────

namespace swordfs::fuse {

int Mount(int argc, char* argv[]) {
  return ::fuse_main(argc, argv, &swordfs_ops, nullptr);
}

} // namespace swordfs::fuse
