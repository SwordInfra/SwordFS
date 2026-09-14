// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE hook factory — static callbacks that forward to SwordFsInterface.

#include "fuse/Vfs.hpp"

#include <dirent.h>
#include <folly/logging/xlog.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "fuse/Limits.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "vfs/VfsImpl.hpp"

using swordfs::vfs::VfsImpl;

namespace swordfs::fuse {

namespace {

template <typename Fn>
void RunFuseInFiber(fuse_req_t req, Fn &&fn) {
  ::swordfs::utils::RunInFiber(
      [fn = std::forward<Fn>(fn)]() mutable {
        ::swordfs::utils::ExpectInFiberDomain();
        fn();
      },
      [req] {
        SWORDFS_LOG_ERROR << "Failed to admit FUSE request into the fiber runtime";
        fuse_reply_err(req, EIO);
      });
}

}  // namespace

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

void VfsHookFactory::SwordFsInit(void *userdata, struct fuse_conn_info *conn) {
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

void VfsHookFactory::SwordFsLookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  RunFuseInFiber(req, [req, parent, name = std::string(name)] {
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

void VfsHookFactory::SwordFsGetattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  (void)fi;
  RunFuseInFiber(req, [req, ino] {
    SetRequestContext(req);
    struct stat attr;
    auto status = VfsImpl::GetAttr(ino, &attr);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_attr(req, &attr, 1.0);
  });
}

void VfsHookFactory::SwordFsSetattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set,
                                    struct fuse_file_info *fi) {
  (void)fi;
  const struct stat attr_copy = *attr;
  RunFuseInFiber(req, [req, ino, attr = attr_copy, to_set]() mutable {
    SetRequestContext(req);
    struct stat out_attr;
    auto status = VfsImpl::SetAttr(ino, &attr, to_set, &out_attr);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_attr(req, &out_attr, 1.0);
  });
}

void VfsHookFactory::SwordFsReadlink(fuse_req_t req, fuse_ino_t ino) {
  RunFuseInFiber(req, [req, ino] {
    SetRequestContext(req);
    std::string target;
    auto status = VfsImpl::ReadLink(ino, &target);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_readlink(req, target.c_str());
  });
}

void VfsHookFactory::SwordFsMknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev) {
  RunFuseInFiber(req, [req, parent, name = std::string(name), mode, rdev] {
    SetRequestContext(req);
    auto status = VfsImpl::MkNod(parent, name.c_str(), mode, rdev);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsMkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
  RunFuseInFiber(req, [req, parent, name = std::string(name), mode] {
    SetRequestContext(req);
    fuse_entry_param entry;
    auto status = VfsImpl::MkDir(parent, name.c_str(), mode, &entry);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_entry(req, &entry);
  });
}

void VfsHookFactory::SwordFsUnlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  RunFuseInFiber(req, [req, parent, name = std::string(name)] {
    SetRequestContext(req);
    auto status = VfsImpl::Unlink(parent, name.c_str());
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsRmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  RunFuseInFiber(req, [req, parent, name = std::string(name)] {
    SetRequestContext(req);
    auto status = VfsImpl::RmDir(parent, name.c_str());
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsSymlink(fuse_req_t req, const char *link, fuse_ino_t parent, const char *name) {
  RunFuseInFiber(req, [req, link = std::string(link), parent, name = std::string(name)] {
    SetRequestContext(req);
    fuse_entry_param entry{};
    auto status = VfsImpl::Symlink(link.c_str(), parent, name.c_str(), &entry);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_entry(req, &entry);
  });
}

void VfsHookFactory::SwordFsRename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent,
                                   const char *newname, unsigned int flags) {
  RunFuseInFiber(req, [req, parent, name = std::string(name), newparent, newname = std::string(newname), flags] {
    SetRequestContext(req);
    auto status = VfsImpl::Rename(parent, name.c_str(), newparent, newname.c_str(), flags);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsLink(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname) {
  RunFuseInFiber(req, [req, ino, newparent, newname = std::string(newname)] {
    SetRequestContext(req);
    fuse_entry_param entry{};
    auto status = VfsImpl::Link(ino, newparent, newname.c_str(), &entry);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_entry(req, &entry);
  });
}

void VfsHookFactory::SwordFsOpen(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  // Copy |fi| by value — the caller's stack frame is gone by the
  // time the fiber executes on the driver thread.
  RunFuseInFiber(req, [req, ino, fi = *fi]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::Open(ino, &fi);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_open(req, &fi);
  });
}

void VfsHookFactory::SwordFsRead(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, size, off, fh] {
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

void VfsHookFactory::SwordFsWrite(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
                                  struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, buf = folly::IOBuf::copyBuffer(buf, size), off, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::Write(ino, *buf, off, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_write(req, buf->length());
  });
}

void VfsHookFactory::SwordFsFlush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::Flush(ino, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_err(req, 0);
  });
}

void VfsHookFactory::SwordFsRelease(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::Release(ino, fh);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
  uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, datasync, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::FSync(ino, datasync, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_err(req, 0);
  });
}

void VfsHookFactory::SwordFsOpendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  // Copy |fi| by value — the caller's stack frame is gone by the
  // time the fiber executes on the driver thread.
  RunFuseInFiber(req, [req, ino, fi = *fi]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::OpenDir(ino, &fi.fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_open(req, &fi);
  });
}

void VfsHookFactory::SwordFsReaddir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
  const uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, size, off, fh] {
    SetRequestContext(req);
    std::string buf;
    auto status = VfsImpl::ReadDir(req, ino, size, off, fh, &buf);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_buf(req, buf.data(), buf.size());
  });
}

void VfsHookFactory::SwordFsReleasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
  const uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, fh] {
    SetRequestContext(req);
    auto status = VfsImpl::ReleaseDir(ino, fh);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
  (void)fi;
  RunFuseInFiber(req, [req, ino, datasync] {
    SetRequestContext(req);
    auto status = VfsImpl::FSyncDir(ino, datasync);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsStatfs(fuse_req_t req, fuse_ino_t ino) {
  RunFuseInFiber(req, [req, ino] {
    SetRequestContext(req);
    struct statvfs stbuf;
    auto status = VfsImpl::StatFs(ino, &stbuf);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_statfs(req, &stbuf);
  });
}

void VfsHookFactory::SwordFsSetxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size,
                                     int flags) {
  RunFuseInFiber(req, [req, ino, name = std::string(name), value = std::string(value, size), size, flags] {
    SetRequestContext(req);
    auto status = VfsImpl::SetXAttr(ino, name.c_str(), value.data(), size, flags);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsGetxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
  RunFuseInFiber(req, [req, ino, name = std::string(name), size] {
    SetRequestContext(req);
    auto status = VfsImpl::GetXAttr(ino, name.c_str(), size);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsListxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  RunFuseInFiber(req, [req, ino, size] {
    SetRequestContext(req);
    auto status = VfsImpl::ListXAttr(ino, size);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsRemovexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
  RunFuseInFiber(req, [req, ino, name = std::string(name)] {
    SetRequestContext(req);
    auto status = VfsImpl::RemoveXAttr(ino, name.c_str());
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  RunFuseInFiber(req, [req, ino, mask] {
    SetRequestContext(req);
    auto status = VfsImpl::Access(ino, mask);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsCreate(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
                                   struct fuse_file_info *fi) {
  // Copy |fi| by value — the caller's stack frame is gone by the
  // time the fiber executes on the driver thread.
  RunFuseInFiber(req, [req, parent, name = std::string(name), mode, fi = *fi]() mutable {
    SetRequestContext(req);
    fuse_entry_param entry;
    auto status = VfsImpl::Create(parent, name.c_str(), mode, &entry, &fi);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_create(req, &entry, &fi);
  });
}

void VfsHookFactory::SwordFsGetlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, struct flock *lock) {
  (void)ino;
  (void)fi;
  (void)lock;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsSetlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, struct flock *lock,
                                  int sleep) {
  (void)ino;
  (void)fi;
  (void)lock;
  (void)sleep;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsBmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize, uint64_t idx) {
  (void)ino;
  (void)blocksize;
  (void)idx;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsIoctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd, void *arg,
                                  struct fuse_file_info *fi, unsigned flags, const void *in_buf, size_t in_bufsz,
                                  size_t out_bufsz) {
  // libfuse's |arg| is the ioctl argument value/address token used by the
  // ioctl retry protocol; it is not a daemon-owned buffer to dereference.
  // |fi| and |in_buf| are borrowed callback data, so copy those before the
  // request is deferred to the fiber runtime.
  std::string in_buf_copy;
  if (in_buf != nullptr && in_bufsz > 0) {
    in_buf_copy.assign(static_cast<const char *>(in_buf), in_bufsz);
  }
  RunFuseInFiber(req,
                 [req, ino, cmd, arg, fi = *fi, flags, in_buf = std::move(in_buf_copy), in_bufsz, out_bufsz]() mutable {
                   SetRequestContext(req);
                   auto status = VfsImpl::IoCtl(ino, static_cast<int>(cmd), arg, &fi, flags,
                                                in_buf.empty() ? nullptr : in_buf.data(), in_bufsz, out_bufsz);
                   fuse_reply_err(req, status.ToErrno());
                 });
}

void VfsHookFactory::SwordFsPoll(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
                                 struct fuse_pollhandle *ph) {
  (void)ino;
  (void)fi;
  (void)ph;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsWriteBuf(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv, off_t off,
                                     struct fuse_file_info *fi) {
  (void)ino;
  (void)bufv;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsRetrieveReply(fuse_req_t req, void *cookie, fuse_ino_t ino, off_t offset,
                                          struct fuse_bufvec *bufv) {
  auto status = VfsImpl::RetrieveReply(req, cookie, ino, offset, bufv);
  fuse_reply_err(req, status.ToErrno());
}

void VfsHookFactory::SwordFsFlock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, int op) {
  RunFuseInFiber(req, [req, ino, fi = *fi, op]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::FLock(ino, &fi, op);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFallocate(fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length,
                                      struct fuse_file_info *fi) {
  RunFuseInFiber(req, [req, ino, mode, offset, length, fi = *fi]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::FAllocate(ino, mode, offset, length, &fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsReaddirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                                        struct fuse_file_info *fi) {
  const uint64_t fh = fi->fh;
  RunFuseInFiber(req, [req, ino, size, off, fh] {
    SetRequestContext(req);
    std::string buf;
    auto status = VfsImpl::ReadDirPlus(req, ino, size, off, fh, &buf);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_buf(req, buf.data(), buf.size());
  });
}

void VfsHookFactory::SwordFsCopyFileRange(fuse_req_t req, fuse_ino_t ino_in, off_t off_in, struct fuse_file_info *fi_in,
                                          fuse_ino_t ino_out, off_t off_out, struct fuse_file_info *fi_out, size_t len,
                                          int flags) {
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

void VfsHookFactory::SwordFsLseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence, struct fuse_file_info *fi) {
  RunFuseInFiber(req, [req, ino, off, whence, fi = *fi]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::LSeek(ino, off, whence, &fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsTmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode, struct fuse_file_info *fi) {
  RunFuseInFiber(req, [req, parent, mode, fi = *fi]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::TmpFile(parent, mode, &fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsStatx(fuse_req_t req, fuse_ino_t ino, int flags, int mask, struct fuse_file_info *fi) {
  RunFuseInFiber(req, [req, ino, flags, mask, fi = *fi]() mutable {
    SetRequestContext(req);
    auto status = VfsImpl::StatX(ino, flags, mask, &fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

// Operation table
//
// FORGET notifications are intentionally not handled. SwordFS currently has no
// inode-scoped runtime resource whose lifetime depends on FUSE lookup counts.
// If that changes (for example, cache eviction tied to inode lifetime), add an
// explicit FORGET lifecycle policy rather than treating these callbacks as
// mandatory bookkeeping.

const struct fuse_lowlevel_ops &VfsHookFactory::get_ops() {
  static const struct fuse_lowlevel_ops kOps = {
      .init = SwordFsInit,
      .destroy = SwordFsDestroy,
      .lookup = SwordFsLookup,
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
