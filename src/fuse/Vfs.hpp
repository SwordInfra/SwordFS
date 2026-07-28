// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE filesystem — bridge between FUSE kernel interface and SwordFS
// VFS.
//
// Uses the libfuse low-level API (fuse_lowlevel_ops) which operates at the
// inode level rather than the path level.

#pragma once

#include <memory>

#include "fuse/VfsImpl.hpp"
#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cstdint>

namespace swordfs::volume {
class VolumeImpl;
}

namespace swordfs::fuse {

/// Static registry of FUSE callbacks that forward every request to a
/// VfsImpl instance bound to a VolumeImpl.
class VfsHookFactory {
 public:
  /// Return the fully-populated fuse_lowlevel_ops table.
  static const struct fuse_lowlevel_ops& get_ops();

  /// Bind a VolumeImpl to the VfsImpl singleton before mount.
  /// Takes ownership of |vol|.
  static void BindVolume(std::unique_ptr<volume::VolumeImpl> vol);

  // FUSE callbacks — delegate to vfs_
  static void SwordFsInit(void* userdata, struct fuse_conn_info* conn);
  static void SwordFsDestroy(void* userdata);
  static void SwordFsLookup(fuse_req_t req, fuse_ino_t parent,
                            const char* name);
  static void SwordFsForget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup);
  static void SwordFsGetattr(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info* fi);
  static void SwordFsSetattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                             int to_set, struct fuse_file_info* fi);
  static void SwordFsReadlink(fuse_req_t req, fuse_ino_t ino);
  static void SwordFsMknod(fuse_req_t req, fuse_ino_t parent, const char* name,
                           mode_t mode, dev_t rdev);
  static void SwordFsMkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
                           mode_t mode);
  static void SwordFsUnlink(fuse_req_t req, fuse_ino_t parent,
                            const char* name);
  static void SwordFsRmdir(fuse_req_t req, fuse_ino_t parent,
                           const char* name);
  static void SwordFsSymlink(fuse_req_t req, const char* link,
                             fuse_ino_t parent, const char* name);
  static void SwordFsRename(fuse_req_t req, fuse_ino_t parent,
                            const char* name, fuse_ino_t newparent,
                            const char* newname, unsigned int flags);
  static void SwordFsLink(fuse_req_t req, fuse_ino_t ino,
                          fuse_ino_t newparent, const char* newname);
  static void SwordFsOpen(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info* fi);
  static void SwordFsRead(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info* fi);
  static void SwordFsWrite(fuse_req_t req, fuse_ino_t ino, const char* buf,
                           size_t size, off_t off, struct fuse_file_info* fi);
  static void SwordFsFlush(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi);
  static void SwordFsRelease(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info* fi);
  static void SwordFsFsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                           struct fuse_file_info* fi);
  static void SwordFsOpendir(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info* fi);
  static void SwordFsReaddir(fuse_req_t req, fuse_ino_t ino, size_t size,
                             off_t off, struct fuse_file_info* fi);
  static void SwordFsReleasedir(fuse_req_t req, fuse_ino_t ino,
                                struct fuse_file_info* fi);
  static void SwordFsFsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                              struct fuse_file_info* fi);
  static void SwordFsStatfs(fuse_req_t req, fuse_ino_t ino);
  static void SwordFsSetxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                              const char* value, size_t size, int flags);
  static void SwordFsGetxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                              size_t size);
  static void SwordFsListxattr(fuse_req_t req, fuse_ino_t ino, size_t size);
  static void SwordFsRemovexattr(fuse_req_t req, fuse_ino_t ino,
                                 const char* name);
  static void SwordFsAccess(fuse_req_t req, fuse_ino_t ino, int mask);
  static void SwordFsCreate(fuse_req_t req, fuse_ino_t parent, const char* name,
                            mode_t mode, struct fuse_file_info* fi);
  static void SwordFsGetlk(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi, struct flock* lock);
  static void SwordFsSetlk(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi, struct flock* lock,
                           int sleep);
  static void SwordFsBmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize,
                          uint64_t idx);
  static void SwordFsIoctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd, void* arg,
                           struct fuse_file_info* fi, unsigned flags,
                           const void* in_buf, size_t in_bufsz,
                           size_t out_bufsz);
  static void SwordFsPoll(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info* fi,
                          struct fuse_pollhandle* ph);
  static void SwordFsWriteBuf(fuse_req_t req, fuse_ino_t ino,
                              struct fuse_bufvec* bufv, off_t off,
                              struct fuse_file_info* fi);
  static void SwordFsRetrieveReply(fuse_req_t req, void* cookie,
                                   fuse_ino_t ino, off_t offset,
                                   struct fuse_bufvec* bufv);
  static void SwordFsForgetMulti(fuse_req_t req, size_t count,
                                 struct fuse_forget_data* forgets);
  static void SwordFsFlock(fuse_req_t req, fuse_ino_t ino,
                           struct fuse_file_info* fi, int op);
  static void SwordFsFallocate(fuse_req_t req, fuse_ino_t ino, int mode,
                               off_t offset, off_t length,
                               struct fuse_file_info* fi);
  static void SwordFsReaddirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                                 off_t off, struct fuse_file_info* fi);
  static void SwordFsCopyFileRange(fuse_req_t req, fuse_ino_t ino_in,
                                   off_t off_in, struct fuse_file_info* fi_in,
                                   fuse_ino_t ino_out, off_t off_out,
                                   struct fuse_file_info* fi_out, size_t len,
                                   int flags);
  static void SwordFsLseek(fuse_req_t req, fuse_ino_t ino, off_t off,
                           int whence, struct fuse_file_info* fi);
  static void SwordFsTmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
                             struct fuse_file_info* fi);
  static void SwordFsStatx(fuse_req_t req, fuse_ino_t ino, int flags, int mask,
                           struct fuse_file_info* fi);

 private:
  static VfsImpl* vfs_;
};

}  // namespace swordfs::fuse
