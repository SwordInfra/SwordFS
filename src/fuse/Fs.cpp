// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS VFS.
//
// Uses the libfuse low-level API (fuse_lowlevel_ops) which operates at the
// inode level rather than the path level. This avoids the path-to-inode
// translation overhead of the high-level API and is the standard approach
// for performance-sensitive filesystems.
//
// Current phase: minimal implementation — only the operations required for
// a successful mount. All I/O operations return -ENOSYS or -ENOENT.

#include "fuse/Fs.hpp"

#define FUSE_USE_VERSION 31
#include <fuse_lowlevel.h>

#include <cstring>
#include <cstdlib>
#include <cerrno>

#include "utils/Logging.hpp"

// ── Forward declarations ───────────────────────────────────────────────

static void SwordfsInit(void* userdata, struct fuse_conn_info* conn);
static void SwordfsDestroy(void* userdata);
static void SwordfsLookup(fuse_req_t req, fuse_ino_t parent, const char* name);
static void SwordfsGetattr(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi);
static void SwordfsReaddir(fuse_req_t req, fuse_ino_t ino, size_t size,
                           off_t off, struct fuse_file_info* fi);
static void SwordfsStatfs(fuse_req_t req, fuse_ino_t ino);
static void SwordfsAccess(fuse_req_t req, fuse_ino_t ino, int mask);

// ── Helper: fill stat for the root inode ───────────────────────────────

static void FillRootStat(struct stat* stbuf) {
  std::memset(stbuf, 0, sizeof(struct stat));
  stbuf->st_ino  = FUSE_ROOT_ID;
  stbuf->st_mode = S_IFDIR | 0755;
  stbuf->st_nlink = 2;
}

// ── FUSE low-level operations ──────────────────────────────────────────

static void SwordfsInit(void* userdata, struct fuse_conn_info* conn) {
  (void)userdata;

  // Disable FUSE_INTERRUPT — we don't support it yet, and turning it off
  // avoids unnecessary lock contention in the kernel.
  conn->no_interrupt = 1;

  SWORDFS_LOG_INFO << "SwordFS filesystem initialized (mount OK)";
}

static void SwordfsDestroy(void* userdata) {
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
}

static void SwordfsLookup(fuse_req_t req, fuse_ino_t parent,
                          const char* name) {
  (void)parent;
  (void)name;

  // Root is the only inode; everything else does not exist.
  fuse_reply_err(req, ENOENT);
}

static void SwordfsGetattr(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi) {
  (void)fi;

  struct stat stbuf;
  FillRootStat(&stbuf);

  if (ino == FUSE_ROOT_ID) {
    stbuf.st_ino = ino;
    fuse_reply_attr(req, &stbuf, 1.0);
  } else {
    fuse_reply_err(req, ENOENT);
  }
}

static void SwordfsReaddir(fuse_req_t req, fuse_ino_t ino, size_t size,
                           off_t off, struct fuse_file_info* fi) {
  (void)fi;

  if (ino != FUSE_ROOT_ID) {
    fuse_reply_err(req, ENOTDIR);
    return;
  }

  struct stat st;
  FillRootStat(&st);

  // Compute buffer size
  size_t buf_size = 0;
  buf_size += fuse_add_direntry(req, nullptr, 0, ".", nullptr, 0);
  buf_size += fuse_add_direntry(req, nullptr, 0, "..", nullptr, 0);

  char* buf = static_cast<char*>(std::malloc(buf_size));
  size_t pos = 0;

  st.st_ino = FUSE_ROOT_ID;
  pos += fuse_add_direntry(req, buf + pos, buf_size - pos, ".", &st, buf_size);
  // ".." for root is root itself
  pos += fuse_add_direntry(req, buf + pos, buf_size - pos, "..", &st, buf_size);

  if (static_cast<size_t>(off) < buf_size) {
    size_t remain = buf_size - static_cast<size_t>(off);
    size_t reply_sz = remain < size ? remain : size;
    fuse_reply_buf(req, buf + static_cast<size_t>(off), reply_sz);
  } else {
    fuse_reply_buf(req, nullptr, 0);
  }

  std::free(buf);
}

static void SwordfsStatfs(fuse_req_t req, fuse_ino_t ino) {
  (void)ino;

  struct statvfs stbuf;
  std::memset(&stbuf, 0, sizeof(struct statvfs));
  stbuf.f_namemax = 255;
  stbuf.f_frsize  = 4096;
  stbuf.f_bsize   = 4096;

  fuse_reply_statfs(req, &stbuf);
}

static void SwordfsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  (void)ino;
  (void)mask;

  // Allow all access for now (single-user filesystem).
  fuse_reply_err(req, 0);
}

// ── Operation table ────────────────────────────────────────────────────

static const struct fuse_lowlevel_ops swordfs_ll_ops = {
  .init      = SwordfsInit,
  .destroy   = SwordfsDestroy,
  .lookup    = SwordfsLookup,
  .getattr   = SwordfsGetattr,
  .readdir   = SwordfsReaddir,
  .statfs    = SwordfsStatfs,
  .access    = SwordfsAccess,
};

// ── Public API ─────────────────────────────────────────────────────────

namespace swordfs::fuse {

int Mount(int argc, char* argv[]) {
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

  // Create low-level session — this parses remaining FUSE options.
  struct fuse_session* se =
      fuse_session_new(&args, &swordfs_ll_ops, sizeof(swordfs_ll_ops), nullptr);
  if (se == nullptr) {
    fuse_opt_free_args(&args);
    return 1;
  }

  // After fuse_session_new, exactly one positional arg should remain
  // (the mountpoint).
  if (args.argc != 1 || args.argv[0][0] == '-') {
    fuse_log(FUSE_LOG_ERR, "error: no mountpoint specified\n");
    fuse_session_destroy(se);
    fuse_opt_free_args(&args);
    return 1;
  }
  const char* mountpoint = args.argv[0];

  // Set up signal handlers so that SIGINT/SIGTERM/HUP cleanly exit the loop.
  if (fuse_set_signal_handlers(se) != 0) {
    fuse_session_destroy(se);
    fuse_opt_free_args(&args);
    return 1;
  }

  // Mount the session.
  if (fuse_session_mount(se, mountpoint) != 0) {
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    fuse_opt_free_args(&args);
    return 1;
  }

  // Enter the event loop (blocks until unmounted or signalled).
  int ret = fuse_session_loop(se);

  // Cleanup.
  fuse_session_unmount(se);
  fuse_remove_signal_handlers(se);
  fuse_session_destroy(se);
  fuse_opt_free_args(&args);

  return ret ? 1 : 0;
}

}  // namespace swordfs::fuse
