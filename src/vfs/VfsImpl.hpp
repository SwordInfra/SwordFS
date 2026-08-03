// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VfsImpl — default VFS implementation.  Delegates to MetaStore and
// translates Status-wrapped results into fuse_reply_* calls.
// The VfsHookFactory layer only needs to wrap each call in RunInFiber.

#pragma once

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs {

namespace volume {
class VolumeImpl;
}

namespace vfs {

class VfsImpl {
 public:
  /// Return the VolumeImpl singleton (convenience).
  static volume::VolumeImpl *Volume();

  static utils::Status Lookup(fuse_ino_t parent, const char *name,
                              fuse_entry_param *entry);
  static void Forget(fuse_ino_t ino, uint64_t nlookup);
  static utils::Status Getattr(fuse_ino_t ino, struct stat *attr);
  static utils::Status Setattr(fuse_ino_t ino, struct stat *attr, int to_set,
                               struct stat *out_attr);
  static utils::Status Readlink(fuse_ino_t ino);
  static utils::Status Mknod(fuse_ino_t parent, const char *name,
                             mode_t mode, dev_t rdev);
  static utils::Status Mkdir(fuse_ino_t parent, const char *name, mode_t mode,
                             fuse_entry_param *entry);
  static utils::Status Unlink(fuse_ino_t parent, const char *name);
  static utils::Status Rmdir(fuse_ino_t parent, const char *name);
  static utils::Status Symlink(const char *link, fuse_ino_t parent,
                               const char *name);
  static utils::Status Rename(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags);
  static utils::Status Link(fuse_ino_t ino, fuse_ino_t newparent,
                            const char *newname);
  static utils::Status Open(fuse_ino_t ino, struct fuse_file_info *fi);
  static utils::Status Read(fuse_ino_t ino, size_t size, off_t off, uint64_t fh,
                            std::unique_ptr<folly::IOBuf> *data);
  static utils::Status Write(fuse_ino_t ino, const folly::IOBuf &buf,
                             off_t off, uint64_t fh);
  static utils::Status Flush(fuse_ino_t ino, uint64_t fh);
  static utils::Status Release(fuse_ino_t ino, uint64_t fh);
  static utils::Status Fsync(fuse_ino_t ino, int datasync, uint64_t fh);
  static utils::Status Opendir(fuse_ino_t ino, uint64_t *fh);
  static utils::Status Readdir(fuse_ino_t ino, size_t size, off_t off,
                               std::string *buf);
  static utils::Status Releasedir(fuse_ino_t ino, uint64_t fh);
  static utils::Status Fsyncdir(fuse_ino_t ino, int datasync);
  static utils::Status Statfs(fuse_ino_t ino, struct statvfs *stbuf);
  static utils::Status Setxattr(fuse_ino_t ino, const char *name,
                                const char *value, size_t size, int flags);
  static utils::Status Getxattr(fuse_ino_t ino, const char *name, size_t size);
  static utils::Status Listxattr(fuse_ino_t ino, size_t size);
  static utils::Status Removexattr(fuse_ino_t ino, const char *name);
  static utils::Status Access(fuse_ino_t ino, int mask);
  static utils::Status Create(fuse_ino_t parent, const char *name, mode_t mode,
                              fuse_entry_param *entry,
                              struct fuse_file_info *fi);
  static utils::Status Ioctl(fuse_ino_t ino, int cmd, void *arg,
                             struct fuse_file_info *fi, unsigned flags,
                             const void *in_buf, size_t in_bufsz,
                             size_t out_bufsz);
  static utils::Status RetrieveReply(fuse_req_t req, void *cookie,
                                     fuse_ino_t ino, off_t offset,
                                     struct fuse_bufvec *bufv);
  static void ForgetMulti(fuse_req_t req, size_t count,
                          struct fuse_forget_data *forgets);
  static utils::Status Flock(fuse_ino_t ino, struct fuse_file_info *fi, int op);
  static utils::Status Fallocate(fuse_ino_t ino, int mode, off_t offset,
                                 off_t length, struct fuse_file_info *fi);
  static utils::Status Readdirplus(fuse_ino_t ino, size_t size, off_t off,
                                   std::string *buf);
  static utils::Status Lseek(fuse_ino_t ino, off_t off, int whence,
                             struct fuse_file_info *fi);
  static utils::Status Tmpfile(fuse_ino_t parent, mode_t mode,
                               struct fuse_file_info *fi);
  static utils::Status Statx(fuse_ino_t ino, int flags, int mask,
                             struct fuse_file_info *fi);
};

}  // namespace vfs
}  // namespace swordfs
