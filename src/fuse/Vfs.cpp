// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE hook factory — static callbacks that forward to SwordFsInterface.

#include "fuse/Vfs.hpp"

#include <dirent.h>
#include <folly/logging/xlog.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "fuse/Limits.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "vfs/VfsImpl.hpp"

using swordfs::vfs::VfsImpl;

namespace swordfs::fuse {

// ────────────────────────────────────────────────────────────────
// Per-request context setup
// ────────────────────────────────────────────────────────────────

void VfsHookFactory::SetRequestContext(fuse_req_t req) {
  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx = SwordFsContext{fuse_req_ctx(req)};
}

// ────────────────────────────────────────────────────────────────
// FUSE callbacks: forward to `VfsImpl`.
// ────────────────────────────────────────────────────────────────

void VfsHookFactory::SwordFsInit(void *userdata,
                                 struct fuse_conn_info *conn) {
  // Initialise per-thread fiber runtime.
  ::swordfs::utils::InitFiberRuntime();
  (void)userdata;
  conn->no_interrupt = 1;
  conn->max_write = kMaxWriteSize;
  conn->max_readahead = kMaxReadAheadSize;
  conn->time_gran = kTimeGran;

  // Writeback cache is intentionally disabled: with it enabled the kernel
  // answers writes from its own page cache, which masks daemon-side
  // semantics (e.g. rejecting writes to flushed chunks, open-unlink).
  fuse_unset_feature_flag(conn, FUSE_CAP_WRITEBACK_CACHE);

  if (conn->capable & FUSE_CAP_SPLICE_READ) {
    fuse_set_feature_flag(conn, FUSE_CAP_SPLICE_READ);
  }
  if (conn->capable & FUSE_CAP_READDIRPLUS) {
    fuse_set_feature_flag(conn, FUSE_CAP_READDIRPLUS);
  }
  if (conn->capable & FUSE_CAP_ASYNC_READ) {
    fuse_set_feature_flag(conn, FUSE_CAP_ASYNC_READ);
  }
  if (conn->capable & FUSE_CAP_ATOMIC_O_TRUNC) {
    fuse_set_feature_flag(conn, FUSE_CAP_ATOMIC_O_TRUNC);
  }

  fuse_unset_feature_flag(conn, FUSE_CAP_SPLICE_WRITE);

  SWORDFS_LOG_INFO << "SwordFS filesystem initialized (mount OK)";
}

void VfsHookFactory::SwordFsDestroy(void *userdata) {
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
  ::swordfs::utils::ShutdownFiberRuntime();
}

void VfsHookFactory::SwordFsLookup(fuse_req_t req, fuse_ino_t parent,
                                   const char *name) {
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name)] {
        SetRequestContext(req);
        fuse_entry_param entry;
        auto status = VfsImpl::Lookup(parent, name.c_str(), &entry);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_entry(req, &entry);
      });
}

void VfsHookFactory::SwordFsForget(fuse_req_t req, fuse_ino_t ino,
                                   uint64_t nlookup) {
  ::swordfs::utils::RunInFiber(
      [ino, nlookup] { VfsImpl::Forget(ino, nlookup); });
}

void VfsHookFactory::SwordFsGetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info *fi) {
  (void)fi;
  ::swordfs::utils::RunInFiber([req, ino] {
    SetRequestContext(req);
    struct stat attr;
    auto status = VfsImpl::Getattr(ino, &attr);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_attr(req, &attr, 1.0);
  });
}

void VfsHookFactory::SwordFsSetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct stat *attr, int to_set,
                                    struct fuse_file_info *fi) {
  (void)fi;
  ::swordfs::utils::RunInFiber([req, ino, attr, to_set] {
    SetRequestContext(req);
    struct stat out_attr;
    auto status = VfsImpl::Setattr(ino, attr, to_set, &out_attr);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_attr(req, &out_attr, 1.0);
  });
}

void VfsHookFactory::SwordFsReadlink(fuse_req_t req, fuse_ino_t ino) {
  ::swordfs::utils::RunInFiber([req, ino] {
    SetRequestContext(req);
    std::string target;
    auto status = VfsImpl::Readlink(ino, &target);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_readlink(req, target.c_str());
  });
}

void VfsHookFactory::SwordFsMknod(fuse_req_t req, fuse_ino_t parent,
                                  const char *name, mode_t mode,
                                  dev_t rdev) {
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name), mode, rdev] {
        SetRequestContext(req);
        auto status = VfsImpl::Mknod(parent, name.c_str(), mode, rdev);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsMkdir(fuse_req_t req, fuse_ino_t parent,
                                  const char *name, mode_t mode) {
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name), mode] {
        SetRequestContext(req);
        fuse_entry_param entry;
        auto status = VfsImpl::Mkdir(parent, name.c_str(), mode, &entry);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_entry(req, &entry);
      });
}

void VfsHookFactory::SwordFsUnlink(fuse_req_t req, fuse_ino_t parent,
                                   const char *name) {
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name)] {
        SetRequestContext(req);
        auto status = VfsImpl::Unlink(parent, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsRmdir(fuse_req_t req, fuse_ino_t parent,
                                  const char *name) {
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name)] {
        SetRequestContext(req);
        auto status = VfsImpl::Rmdir(parent, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsSymlink(fuse_req_t req, const char *link,
                                    fuse_ino_t parent, const char *name) {
  ::swordfs::utils::RunInFiber(
      [req, link = std::string(link), parent,
       name = std::string(name)] {
        SetRequestContext(req);
        fuse_entry_param entry{};
        auto status = VfsImpl::Symlink(link.c_str(), parent,
                                       name.c_str(), &entry);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_entry(req, &entry);
      });
}

void VfsHookFactory::SwordFsRename(fuse_req_t req, fuse_ino_t parent,
                                   const char *name, fuse_ino_t newparent,
                                   const char *newname, unsigned int flags) {
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name), newparent,
       newname = std::string(newname), flags] {
        SetRequestContext(req);
        auto status = VfsImpl::Rename(parent, name.c_str(), newparent,
                                      newname.c_str(), flags);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsLink(fuse_req_t req, fuse_ino_t ino,
                                 fuse_ino_t newparent, const char *newname) {
  ::swordfs::utils::RunInFiber(
      [req, ino, newparent, newname = std::string(newname)] {
        SetRequestContext(req);
        fuse_entry_param entry{};
        auto status = VfsImpl::Link(ino, newparent, newname.c_str(),
                                    &entry);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_entry(req, &entry);
      });
}

void VfsHookFactory::SwordFsOpen(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info *fi) {
  // Copy |fi| by value — the caller's stack frame is gone by the
  // time the fiber executes on the driver thread.
  ::swordfs::utils::RunInFiber(
      [req, ino, fi = *fi]() mutable {
        SetRequestContext(req);
        auto status = VfsImpl::Open(ino, &fi);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_open(req, &fi);
      });
}

void VfsHookFactory::SwordFsRead(fuse_req_t req, fuse_ino_t ino, size_t size,
                                 off_t off, struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([req, ino, size, off, fh] {
    SetRequestContext(req);
    std::unique_ptr<folly::IOBuf> data;
    auto status = VfsImpl::Read(ino, size, off, fh, &data);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_buf(req, reinterpret_cast<const char *>(data->data()), data->length());
  });
}

void VfsHookFactory::SwordFsWrite(fuse_req_t req, fuse_ino_t ino,
                                  const char *buf, size_t size, off_t off,
                                  struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [req, ino, buf = folly::IOBuf::copyBuffer(buf, size), off, fh] {
        SetRequestContext(req);
        auto status = VfsImpl::Write(ino, *buf, off, fh);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_write(req, buf->length());
      });
}

void VfsHookFactory::SwordFsFlush(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([req, ino, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::Flush(ino, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_err(req, 0);
  });
}

void VfsHookFactory::SwordFsRelease(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([req, ino, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::Release(ino, fh);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFsync(fuse_req_t req, fuse_ino_t ino,
                                  int datasync, struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([req, ino, datasync, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::Fsync(ino, datasync, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_err(req, 0);
  });
}

void VfsHookFactory::SwordFsOpendir(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info *fi) {
  // Copy |fi| by value — the caller's stack frame is gone by the
  // time the fiber executes on the driver thread.
  ::swordfs::utils::RunInFiber(
      [req, ino, fi = *fi]() mutable {
        SetRequestContext(req);
        auto status = VfsImpl::Opendir(ino, &fi.fh);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_open(req, &fi);
      });
}

void VfsHookFactory::SwordFsReaddir(fuse_req_t req, fuse_ino_t ino,
                                    size_t size, off_t off,
                                    struct fuse_file_info *fi) {
  (void)fi;
  ::swordfs::utils::RunInFiber([req, ino, size, off] {
    SetRequestContext(req);
    std::string buf;
    auto status = VfsImpl::Readdir(req, ino, size, off, &buf);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_buf(req, buf.data(), buf.size());
  });
}

void VfsHookFactory::SwordFsReleasedir(fuse_req_t req, fuse_ino_t ino,
                                       struct fuse_file_info *fi) {
  ::swordfs::utils::RunInFiber([req, ino, fi] {
    SetRequestContext(req);
    auto status = VfsImpl::Releasedir(ino, fi->fh);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFsyncdir(fuse_req_t req, fuse_ino_t ino,
                                     int datasync,
                                     struct fuse_file_info *fi) {
  (void)fi;
  ::swordfs::utils::RunInFiber([req, ino, datasync] {
    SetRequestContext(req);
    auto status = VfsImpl::Fsyncdir(ino, datasync);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsStatfs(fuse_req_t req, fuse_ino_t ino) {
  ::swordfs::utils::RunInFiber([req, ino] {
    SetRequestContext(req);
    struct statvfs stbuf;
    auto status = VfsImpl::Statfs(ino, &stbuf);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_statfs(req, &stbuf);
  });
}

void VfsHookFactory::SwordFsSetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char *name, const char *value,
                                     size_t size, int flags) {
  ::swordfs::utils::RunInFiber(
      [req, ino, name = std::string(name),
       value = std::string(value, size), size, flags] {
        SetRequestContext(req);
        auto status = VfsImpl::Setxattr(ino, name.c_str(), value.data(),
                                        size, flags);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsGetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char *name, size_t size) {
  ::swordfs::utils::RunInFiber(
      [req, ino, name = std::string(name), size] {
        SetRequestContext(req);
        auto status = VfsImpl::Getxattr(ino, name.c_str(), size);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsListxattr(fuse_req_t req, fuse_ino_t ino,
                                      size_t size) {
  ::swordfs::utils::RunInFiber([req, ino, size] {
    SetRequestContext(req);
    auto status = VfsImpl::Listxattr(ino, size);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsRemovexattr(fuse_req_t req, fuse_ino_t ino,
                                        const char *name) {
  ::swordfs::utils::RunInFiber(
      [req, ino, name = std::string(name)] {
        SetRequestContext(req);
        auto status = VfsImpl::Removexattr(ino, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  ::swordfs::utils::RunInFiber([req, ino, mask] {
    SetRequestContext(req);
    auto status = VfsImpl::Access(ino, mask);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsCreate(fuse_req_t req, fuse_ino_t parent,
                                   const char *name, mode_t mode,
                                   struct fuse_file_info *fi) {
  // Copy |fi| by value — the caller's stack frame is gone by the
  // time the fiber executes on the driver thread.
  ::swordfs::utils::RunInFiber(
      [req, parent, name = std::string(name), mode,
       fi = *fi]() mutable {
        SetRequestContext(req);
        fuse_entry_param entry;
        auto status = VfsImpl::Create(parent, name.c_str(), mode,
                                      &entry, &fi);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_create(req, &entry, &fi);
      });
}

void VfsHookFactory::SwordFsGetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi,
                                  struct flock *lock) {
  (void)ino;
  (void)fi;
  (void)lock;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsSetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi,
                                  struct flock *lock, int sleep) {
  (void)ino;
  (void)fi;
  (void)lock;
  (void)sleep;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsBmap(fuse_req_t req, fuse_ino_t ino,
                                 size_t blocksize, uint64_t idx) {
  (void)ino;
  (void)blocksize;
  (void)idx;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsIoctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd,
                                  void *arg, struct fuse_file_info *fi,
                                  unsigned flags, const void *in_buf,
                                  size_t in_bufsz, size_t out_bufsz) {
  std::string in_buf_str;
  if (in_buf && in_bufsz > 0) {
    in_buf_str.assign(static_cast<const char *>(in_buf), in_bufsz);
  }
  ::swordfs::utils::RunInFiber(
      [req, ino, cmd, arg, fi, flags,
       in_buf_str = std::move(in_buf_str), in_bufsz, out_bufsz] {
        SetRequestContext(req);
        auto status = VfsImpl::Ioctl(ino, static_cast<int>(cmd), arg, fi, flags,
                                     in_buf_str.empty() ? nullptr : in_buf_str.data(),
                                     in_bufsz, out_bufsz);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsPoll(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info *fi,
                                 struct fuse_pollhandle *ph) {
  (void)ino;
  (void)fi;
  (void)ph;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsWriteBuf(fuse_req_t req, fuse_ino_t ino,
                                     struct fuse_bufvec *bufv, off_t off,
                                     struct fuse_file_info *fi) {
  (void)ino;
  (void)bufv;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsRetrieveReply(fuse_req_t req, void *cookie,
                                          fuse_ino_t ino, off_t offset,
                                          struct fuse_bufvec *bufv) {
  auto status = VfsImpl::RetrieveReply(req, cookie, ino, offset, bufv);
  fuse_reply_err(req, status.ToErrno());
}

void VfsHookFactory::SwordFsForgetMulti(fuse_req_t req, size_t count,
                                        struct fuse_forget_data *forgets) {
  std::vector<fuse_forget_data> forgets_copy(forgets, forgets + count);
  ::swordfs::utils::RunInFiber(
      [req, count,
       forgets_copy = std::move(forgets_copy)]() mutable {
        VfsImpl::ForgetMulti(req, count, forgets_copy.data());
      });
}

void VfsHookFactory::SwordFsFlock(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi, int op) {
  ::swordfs::utils::RunInFiber([req, ino, fi, op] {
    SetRequestContext(req);
    auto status = VfsImpl::Flock(ino, fi, op);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFallocate(fuse_req_t req, fuse_ino_t ino,
                                      int mode, off_t offset, off_t length,
                                      struct fuse_file_info *fi) {
  ::swordfs::utils::RunInFiber(
      [req, ino, mode, offset, length, fi] {
        SetRequestContext(req);
        auto status = VfsImpl::Fallocate(ino, mode, offset, length, fi);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsReaddirplus(fuse_req_t req, fuse_ino_t ino,
                                        size_t size, off_t off,
                                        struct fuse_file_info *fi) {
  (void)fi;
  ::swordfs::utils::RunInFiber([req, ino, size, off] {
    SetRequestContext(req);
    std::string buf;
    auto status = VfsImpl::Readdirplus(req, ino, size, off, &buf);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_buf(req, buf.data(), buf.size());
  });
}

void VfsHookFactory::SwordFsCopyFileRange(
    fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
    struct fuse_file_info *fi_in, fuse_ino_t ino_out, off_t off_out,
    struct fuse_file_info *fi_out, size_t len, int flags) {
  (void)ino_in;
  (void)off_in;
  (void)fi_in;
  (void)ino_out;
  (void)off_out;
  (void)fi_out;
  (void)len;
  (void)flags;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsLseek(fuse_req_t req, fuse_ino_t ino, off_t off,
                                  int whence, struct fuse_file_info *fi) {
  ::swordfs::utils::RunInFiber([req, ino, off, whence, fi] {
    SetRequestContext(req);
    auto status = VfsImpl::Lseek(ino, off, whence, fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsTmpfile(fuse_req_t req, fuse_ino_t parent,
                                    mode_t mode, struct fuse_file_info *fi) {
  ::swordfs::utils::RunInFiber([req, parent, mode, fi] {
    SetRequestContext(req);
    auto status = VfsImpl::Tmpfile(parent, mode, fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsStatx(fuse_req_t req, fuse_ino_t ino, int flags,
                                  int mask, struct fuse_file_info *fi) {
  ::swordfs::utils::RunInFiber([req, ino, flags, mask, fi] {
    SetRequestContext(req);
    auto status = VfsImpl::Statx(ino, flags, mask, fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

// Operation table

const struct fuse_lowlevel_ops &VfsHookFactory::get_ops() {
  static const struct fuse_lowlevel_ops kOps = {
      .init = SwordFsInit,
      .destroy = SwordFsDestroy,
      .lookup = SwordFsLookup,
      .forget = SwordFsForget,
      .getattr = SwordFsGetattr,
      .setattr = SwordFsSetattr,
      .readlink = SwordFsReadlink,
      .mknod = SwordFsMknod,
      .mkdir = SwordFsMkdir,
      .unlink = SwordFsUnlink,
      .rmdir = SwordFsRmdir,
      .symlink = SwordFsSymlink,
      .rename = SwordFsRename,
      .link = SwordFsLink,
      .open = SwordFsOpen,
      .read = SwordFsRead,
      .write = SwordFsWrite,
      .flush = SwordFsFlush,
      .release = SwordFsRelease,
      .fsync = SwordFsFsync,
      .opendir = SwordFsOpendir,
      .readdir = SwordFsReaddir,
      .releasedir = SwordFsReleasedir,
      .fsyncdir = SwordFsFsyncdir,
      .statfs = SwordFsStatfs,
      .setxattr = SwordFsSetxattr,
      .getxattr = SwordFsGetxattr,
      .listxattr = SwordFsListxattr,
      .removexattr = SwordFsRemovexattr,
      .access = SwordFsAccess,
      .create = SwordFsCreate,
      .getlk = SwordFsGetlk,
      .setlk = SwordFsSetlk,
      .bmap = SwordFsBmap,
      .ioctl = SwordFsIoctl,
      .poll = SwordFsPoll,
      .write_buf = nullptr,  // not implemented — force kernel to use .write
      .retrieve_reply = SwordFsRetrieveReply,
      .forget_multi = SwordFsForgetMulti,
      .flock = SwordFsFlock,
      .fallocate = SwordFsFallocate,
      .readdirplus = SwordFsReaddirplus,
      .copy_file_range = SwordFsCopyFileRange,
      .lseek = SwordFsLseek,
      .tmpfile = SwordFsTmpfile,
      .statx = SwordFsStatx,
  };
  return kOps;
}

}  // namespace swordfs::fuse
