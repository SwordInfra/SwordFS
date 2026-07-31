// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VfsImpl — default VFS implementation.  Delegates to MetaStore and
// translates Status-wrapped results into fuse_reply_* calls.
// The VfsHookFactory layer only needs to wrap each call in RunInFiber.

#pragma once

#define FUSE_USE_VERSION 312
#include <folly/io/IOBuf.h>
#include <fuse_lowlevel.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chunk/ChunkManager.hpp"
#include "config/ConfigCenter.hpp"
#include "utils/Status.hpp"

namespace swordfs {

namespace volume {
class VolumeImpl;
}

namespace vfs {

class VfsImpl {
 public:
  VfsImpl();
  ~VfsImpl();

  /// Initialise with a VolumeImpl, which provides meta engine,
  /// data engine, and volume configuration.  Takes ownership.
  void Init(std::unique_ptr<volume::VolumeImpl> vol);

  /// Return the underlying VolumeImpl for direct meta/data access
  /// (used by Readdir/Readdirplus for fuse_add_direntry wrapping).
  volume::VolumeImpl *Volume() const { return vol_.get(); }

  /// Set per-request fiber context. Must be called before any method
  /// that requires context (caller identity, volume binding).
  void SetRequestContext(fuse_req_t req);

  utils::Status Lookup(fuse_ino_t parent, const char *name,
                       fuse_entry_param *entry);
  void Forget(fuse_ino_t ino, uint64_t nlookup);
  utils::Status Getattr(fuse_ino_t ino, struct stat *attr);
  utils::Status Setattr(fuse_ino_t ino, struct stat *attr, int to_set,
                        struct stat *out_attr);
  utils::Status Readlink(fuse_ino_t ino);
  utils::Status Mknod(fuse_ino_t parent, const char *name,
                      mode_t mode, dev_t rdev);
  utils::Status Mkdir(fuse_ino_t parent, const char *name, mode_t mode,
                      fuse_entry_param *entry);
  utils::Status Unlink(fuse_ino_t parent, const char *name);
  utils::Status Rmdir(fuse_ino_t parent, const char *name);
  utils::Status Symlink(const char *link, fuse_ino_t parent,
                        const char *name);
  utils::Status Rename(fuse_ino_t parent, const char *name,
                       fuse_ino_t newparent, const char *newname,
                       unsigned int flags);
  utils::Status Link(fuse_ino_t ino, fuse_ino_t newparent,
                     const char *newname);
  utils::Status Open(fuse_ino_t ino, struct fuse_file_info *fi);
  utils::Status Read(fuse_ino_t ino, size_t size, off_t off, uint64_t fh,
                     std::unique_ptr<folly::IOBuf> *data);
  utils::Status Write(fuse_ino_t ino, const folly::IOBuf &buf,
                      off_t off, uint64_t fh);
  utils::Status Flush(fuse_ino_t ino, uint64_t fh);
  utils::Status Release(fuse_ino_t ino, uint64_t fh);
  utils::Status Fsync(fuse_ino_t ino, int datasync, uint64_t fh);
  utils::Status Opendir(fuse_ino_t ino, uint64_t *fh);
  utils::Status Readdir(fuse_ino_t ino, size_t size, off_t off,
                        std::string *buf);
  utils::Status Releasedir(fuse_ino_t ino, uint64_t fh);
  utils::Status Fsyncdir(fuse_ino_t ino, int datasync);
  utils::Status Statfs(fuse_ino_t ino, struct statvfs *stbuf);
  utils::Status Setxattr(fuse_ino_t ino, const char *name,
                         const char *value, size_t size, int flags);
  utils::Status Getxattr(fuse_ino_t ino, const char *name, size_t size);
  utils::Status Listxattr(fuse_ino_t ino, size_t size);
  utils::Status Removexattr(fuse_ino_t ino, const char *name);
  utils::Status Access(fuse_ino_t ino, int mask);
  utils::Status Create(fuse_ino_t parent, const char *name, mode_t mode,
                       fuse_entry_param *entry, struct fuse_file_info *fi);
  utils::Status Ioctl(fuse_ino_t ino, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned flags,
                      const void *in_buf, size_t in_bufsz,
                      size_t out_bufsz);
  utils::Status RetrieveReply(fuse_req_t req, void *cookie, fuse_ino_t ino,
                              off_t offset, struct fuse_bufvec *bufv);
  void ForgetMulti(fuse_req_t req, size_t count,
                   struct fuse_forget_data *forgets);
  utils::Status Flock(fuse_ino_t ino, struct fuse_file_info *fi, int op);
  utils::Status Fallocate(fuse_ino_t ino, int mode, off_t offset,
                          off_t length, struct fuse_file_info *fi);
  utils::Status Readdirplus(fuse_ino_t ino, size_t size, off_t off,
                            std::string *buf);
  utils::Status Lseek(fuse_ino_t ino, off_t off, int whence,
                      struct fuse_file_info *fi);
  utils::Status Tmpfile(fuse_ino_t parent, mode_t mode,
                        struct fuse_file_info *fi);
  utils::Status Statx(fuse_ino_t ino, int flags, int mask,
                      struct fuse_file_info *fi);

 private:
  std::unique_ptr<volume::VolumeImpl> vol_;
};

}  // namespace vfs
}  // namespace swordfs
