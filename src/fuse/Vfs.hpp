// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS
// VFS.
//
// Uses the libfuse low-level API (fuse_lowlevel_ops) which operates at the
// inode level rather than the path level.

#pragma once

#include "fuse/VfsImpl.hpp"
#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cstdint>

namespace swordfs::fuse {

/// Static registry of FUSE callbacks that forward every request to a
/// user-provided SwordfsInterface instance.
class VfsHookFactory {
 public:
  /// Return the fully-populated fuse_lowlevel_ops table.
  static const struct fuse_lowlevel_ops& GetOps();

  // FUSE callbacks — delegate to vfs_
  static void SwordfsInit(void* userdata, struct fuse_conn_info* conn);
  static void SwordfsDestroy(void* userdata);
  static void SwordfsLookup(fuse_req_t req, fuse_ino_t parent,
                            const char* name);
  static void SwordfsForget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup);
  static void SwordfsGetattr(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info* fi);
  static void SwordfsSetattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                             int to_set, struct fuse_file_info* fi);
  static void SwordfsReadlink(fuse_req_t req, fuse_ino_t ino);
  static void SwordfsMknod(fuse_req_t req, fuse_ino_t parent, const char* name,
                           mode_t mode, dev_t rdev);
  static void SwordfsMkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
                           mode_t mode);
  static void SwordfsUnlink(fuse_req_t req, fuse_ino_t parent,
                            const char* name);
  static void SwordfsRmdir(fuse_req_t req, fuse_ino_t parent,
                           const char* name);
  static void SwordfsSymlink(fuse_req_t req, const char* link,
                             fuse_ino_t parent, const char* name);
  static void SwordfsRename(fuse_req_t req, fuse_ino_t parent,
                            const char* name, fuse_ino_t newparent,
                            const char* newname, unsigned int flags);
  static void SwordfsLink(fuse_req_t req, fuse_ino_t ino,
                          fuse_ino_t newparent, const char* newname);
  static void SwordfsOpen(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info* fi);
  static void SwordfsRead(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info* fi);
  static void SwordfsWrite(fuse_req_t req, fuse_ino_t ino, const char* buf,
                           size_t size, off_t off, struct fuse_file_info* fi);
  static void SwordfsFlush(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi);
  static void SwordfsRelease(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info* fi);
  static void SwordfsFsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                           struct fuse_file_info* fi);
  static void SwordfsOpendir(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info* fi);
  static void SwordfsReaddir(fuse_req_t req, fuse_ino_t ino, size_t size,
                             off_t off, struct fuse_file_info* fi);
  static void SwordfsReleasedir(fuse_req_t req, fuse_ino_t ino,
                                struct fuse_file_info* fi);
  static void SwordfsFsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                              struct fuse_file_info* fi);
  static void SwordfsStatfs(fuse_req_t req, fuse_ino_t ino);
  static void SwordfsSetxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                              const char* value, size_t size, int flags);
  static void SwordfsGetxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                              size_t size);
  static void SwordfsListxattr(fuse_req_t req, fuse_ino_t ino, size_t size);
  static void SwordfsRemovexattr(fuse_req_t req, fuse_ino_t ino,
                                 const char* name);
  static void SwordfsAccess(fuse_req_t req, fuse_ino_t ino, int mask);
  static void SwordfsCreate(fuse_req_t req, fuse_ino_t parent, const char* name,
                            mode_t mode, struct fuse_file_info* fi);
  static void SwordfsGetlk(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi, struct flock* lock);
  static void SwordfsSetlk(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi, struct flock* lock,
                           int sleep);
  static void SwordfsBmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize,
                          uint64_t idx);
  static void SwordfsIoctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd, void* arg,
                           struct fuse_file_info* fi, unsigned flags,
                           const void* in_buf, size_t in_bufsz,
                           size_t out_bufsz);
  static void SwordfsPoll(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info* fi,
                          struct fuse_pollhandle* ph);
  static void SwordfsWriteBuf(fuse_req_t req, fuse_ino_t ino,
                              struct fuse_bufvec* bufv, off_t off,
                              struct fuse_file_info* fi);
  static void SwordfsRetrieveReply(fuse_req_t req, void* cookie,
                                   fuse_ino_t ino, off_t offset,
                                   struct fuse_bufvec* bufv);
  static void SwordfsForgetMulti(fuse_req_t req, size_t count,
                                 struct fuse_forget_data* forgets);
  static void SwordfsFlock(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi, int op);
  static void SwordfsFallocate(fuse_req_t req, fuse_ino_t ino, int mode,
                               off_t offset, off_t length,
                               struct fuse_file_info* fi);
  static void SwordfsReaddirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                                 off_t off, struct fuse_file_info* fi);
  static void SwordfsCopyFileRange(fuse_req_t req, fuse_ino_t ino_in,
                                   off_t off_in, struct fuse_file_info* fi_in,
                                   fuse_ino_t ino_out, off_t off_out,
                                   struct fuse_file_info* fi_out, size_t len,
                                   int flags);
  static void SwordfsLseek(fuse_req_t req, fuse_ino_t ino, off_t off,
                           int whence, struct fuse_file_info* fi);
  static void SwordfsTmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
                             struct fuse_file_info* fi);
  static void SwordfsStatx(fuse_req_t req, fuse_ino_t ino, int flags, int mask,
                           struct fuse_file_info* fi);

 private:
  static VfsImpl* vfs_;
};

}  // namespace swordfs::fuse
