// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS
// VFS.
//
// Uses the libfuse low-level API (fuse_lowlevel_ops) which operates at the
// inode level rather than the path level. This avoids the path-to-inode
// translation overhead of the high-level API and is the standard approach
// for performance-sensitive filesystems.

#include "fuse/Vfs.hpp"

#define FUSE_USE_VERSION 31
#include <fuse_lowlevel.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "fuse/Limits.hpp"
#include "utils/Logging.hpp"

namespace swordfs::fuse {

// Helper: fill stat for the root inode
static void FillRootStat(struct stat* stbuf) {
  std::memset(stbuf, 0, sizeof(struct stat));
  stbuf->st_ino = FUSE_ROOT_ID;
  stbuf->st_mode = S_IFDIR | 0755;
  stbuf->st_nlink = 2;
}

// FUSE low-level operations

static void SwordfsInit(void* userdata, struct fuse_conn_info* conn) {
  (void)userdata;

  // Disable FUSE_INTERRUPT — we don't support it yet, and turning it off
  // avoids unnecessary lock contention in the kernel.
  conn->no_interrupt = 1;

  // Increase max read/write size for better throughput.
  conn->max_write = kMaxWriteSize;
  conn->max_readahead = kMaxReadAheadSize;

  // Microsecond timestamp granularity (good balance for FUSE).
  conn->time_gran = kTimeGran;

  // Enable capabilities for production-grade performance.
  conn->want |= FUSE_CAP_WRITEBACK_CACHE;
  conn->want |= FUSE_CAP_SPLICE_WRITE;
  conn->want |= FUSE_CAP_SPLICE_READ;
  conn->want |= FUSE_CAP_READDIRPLUS;
  conn->want |= FUSE_CAP_ASYNC_READ;
  conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;
  conn->want |= FUSE_CAP_DONT_MASK;

  SWORDFS_LOG_INFO << "SwordFS filesystem initialized (mount OK)";
}

static void SwordfsDestroy(void* userdata) {
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
}

static void SwordfsLookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  (void)parent;
  (void)name;

  // Root is the only inode; everything else does not exist.
  fuse_reply_err(req, ENOENT);
}

static void SwordfsMkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
                         mode_t mode) {
  (void)parent;
  (void)name;
  (void)mode;

  fuse_reply_err(req, ENOSYS);
}

static void SwordfsCreate(fuse_req_t req, fuse_ino_t parent, const char* name,
                          mode_t mode, struct fuse_file_info* fi) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)fi;

  fuse_reply_err(req, ENOSYS);
}

static void SwordfsUnlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  (void)parent;
  (void)name;

  fuse_reply_err(req, ENOSYS);
}

static void SwordfsRmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  (void)parent;
  (void)name;

  fuse_reply_err(req, ENOSYS);
}

static void SwordfsGetattr(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi) {
  (void)fi;

  struct stat stbuf;
  if (ino == FUSE_ROOT_ID) {
    FillRootStat(&stbuf);
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
  stbuf.f_frsize = 4096;
  stbuf.f_bsize = 4096;

  fuse_reply_statfs(req, &stbuf);
}

static void SwordfsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  (void)ino;
  (void)mask;

  // Allow all access for now (single-user filesystem).
  fuse_reply_err(req, 0);
}

// Operation table
const struct fuse_lowlevel_ops swordfs_ll_ops = {
    .init = SwordfsInit,
    .destroy = SwordfsDestroy,
    .lookup = SwordfsLookup,
    .getattr = SwordfsGetattr,
    .mkdir = SwordfsMkdir,
    .unlink = SwordfsUnlink,
    .rmdir = SwordfsRmdir,
    .readdir = SwordfsReaddir,
    .statfs = SwordfsStatfs,
    .access = SwordfsAccess,
    .create = SwordfsCreate,
};

}  // namespace swordfs::fuse
