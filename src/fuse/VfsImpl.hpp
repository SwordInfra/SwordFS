// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VfsImpl — default VFS implementation.  Delegates to MetaStore and
// translates Status-wrapped results into fuse_reply_* calls.
// The VfsHookFactory layer only needs to wrap each call in RunInFiber.

#pragma once

#define FUSE_USE_VERSION 312
#include <folly/container/F14Map.h>
#include <fuse_lowlevel.h>

#include <memory>
#include <string>

namespace swordfs {

namespace metadata {
class Meta;
}  // namespace metadata

namespace storage {
class IDataEngine;
}  // namespace storage

namespace fuse {

class VfsImpl {
 public:
  VfsImpl();
  ~VfsImpl();

  void Init(void* userdata, struct fuse_conn_info* conn);
  void Destroy(void* userdata);

  void Lookup(fuse_req_t req, fuse_ino_t parent, const char* name);
  void Forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup);
  void Getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
  void Setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
               int to_set, struct fuse_file_info* fi);
  void Readlink(fuse_req_t req, fuse_ino_t ino);
  void Mknod(fuse_req_t req, fuse_ino_t parent, const char* name,
             mode_t mode, dev_t rdev);
  void Mkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
             mode_t mode);
  void Unlink(fuse_req_t req, fuse_ino_t parent, const char* name);
  void Rmdir(fuse_req_t req, fuse_ino_t parent, const char* name);
  void Symlink(fuse_req_t req, const char* link, fuse_ino_t parent,
               const char* name);
  void Rename(fuse_req_t req, fuse_ino_t parent, const char* name,
              fuse_ino_t newparent, const char* newname,
              unsigned int flags);
  void Link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
            const char* newname);
  void Open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
  void Read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            struct fuse_file_info* fi);
  void Write(fuse_req_t req, fuse_ino_t ino, const char* buf,
             size_t size, off_t off, struct fuse_file_info* fi);
  void Flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
  void Release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
  void Fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
             struct fuse_file_info* fi);
  void Opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
  void Readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
               struct fuse_file_info* fi);
  void Releasedir(fuse_req_t req, fuse_ino_t ino,
                  struct fuse_file_info* fi);
  void Fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                struct fuse_file_info* fi);
  void Statfs(fuse_req_t req, fuse_ino_t ino);
  void Setxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                const char* value, size_t size, int flags);
  void Getxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                size_t size);
  void Listxattr(fuse_req_t req, fuse_ino_t ino, size_t size);
  void Removexattr(fuse_req_t req, fuse_ino_t ino, const char* name);
  void Access(fuse_req_t req, fuse_ino_t ino, int mask);
  void Create(fuse_req_t req, fuse_ino_t parent, const char* name,
              mode_t mode, struct fuse_file_info* fi);
  void Ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void* arg,
             struct fuse_file_info* fi, unsigned flags,
             const void* in_buf, size_t in_bufsz, size_t out_bufsz);
  void RetrieveReply(fuse_req_t req, void* cookie, fuse_ino_t ino,
                     off_t offset, struct fuse_bufvec* bufv);
  void ForgetMulti(fuse_req_t req, size_t count,
                   struct fuse_forget_data* forgets);
  void Flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi,
             int op);
  void Fallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset,
                 off_t length, struct fuse_file_info* fi);
  void Readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                   off_t off, struct fuse_file_info* fi);
  void Lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
             struct fuse_file_info* fi);
  void Tmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
               struct fuse_file_info* fi);
  void Statx(fuse_req_t req, fuse_ino_t ino, int flags, int mask,
             struct fuse_file_info* fi);

 private:
  // Helper: derive the chunk key for an inode.  The initial (milestone-3)
  // implementation maps each file to a single chunk keyed by inode number.
  static std::string ChunkKey(InodeID ino);

  std::unique_ptr<swordfs::metadata::Meta> meta_;
  std::unique_ptr<swordfs::storage::IDataEngine> data_;

  // Write buffer: file handle → accumulated bytes not yet flushed.
  folly::F14FastMap<uint64_t, std::string> write_buf_;
};

}  // namespace fuse
}  // namespace swordfs
