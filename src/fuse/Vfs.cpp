// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE hook factory — static callbacks that forward to SwordFsInterface.

#include "fuse/Vfs.hpp"

#include <string>
#include <vector>

#include "fuse/Limits.hpp"
#include "vfs/VfsImpl.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::vfs::VfsImpl;

namespace swordfs::fuse {

VfsImpl* VfsHookFactory::vfs_ = new VfsImpl();

void VfsHookFactory::BindVolume(std::unique_ptr<volume::VolumeImpl> vol) {
  vfs_->Init(std::move(vol));
}

// ────────────────────────────────────────────────────────────────
// FUSE callbacks: forward to `VfsImpl`.
// ────────────────────────────────────────────────────────────────

void VfsHookFactory::SwordFsInit(void* userdata,
                                 struct fuse_conn_info* conn) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  // Initialise per-thread fiber runtime.
  ::swordfs::utils::InitFiberRuntime();
  (void)userdata;
  conn->no_interrupt = 1;
  conn->max_write = kMaxWriteSize;
  conn->max_readahead = kMaxReadAheadSize;
  conn->time_gran = kTimeGran;

  if (conn->capable & FUSE_CAP_WRITEBACK_CACHE)
    fuse_set_feature_flag(conn, FUSE_CAP_WRITEBACK_CACHE);
  if (conn->capable & FUSE_CAP_SPLICE_READ)
    fuse_set_feature_flag(conn, FUSE_CAP_SPLICE_READ);
  if (conn->capable & FUSE_CAP_READDIRPLUS)
    fuse_set_feature_flag(conn, FUSE_CAP_READDIRPLUS);
  if (conn->capable & FUSE_CAP_ASYNC_READ)
    fuse_set_feature_flag(conn, FUSE_CAP_ASYNC_READ);
  if (conn->capable & FUSE_CAP_ATOMIC_O_TRUNC)
    fuse_set_feature_flag(conn, FUSE_CAP_ATOMIC_O_TRUNC);
  if (conn->capable & FUSE_CAP_DONT_MASK)
    fuse_set_feature_flag(conn, FUSE_CAP_DONT_MASK);

  fuse_unset_feature_flag(conn, FUSE_CAP_SPLICE_WRITE);

  SWORDFS_LOG_INFO << "SwordFS filesystem initialized (mount OK)";
}

void VfsHookFactory::SwordFsDestroy(void* userdata) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
  ::swordfs::utils::ShutdownFiberRuntime();
}

void VfsHookFactory::SwordFsLookup(fuse_req_t req, fuse_ino_t parent,
                                   const char* name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->Lookup(req, parent, name.c_str());
      });
}

void VfsHookFactory::SwordFsForget(fuse_req_t req, fuse_ino_t ino,
                                   uint64_t nlookup) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, nlookup] { vfs->Forget(req, ino, nlookup); });
}

void VfsHookFactory::SwordFsGetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Getattr(req, ino, fi); });
}

void VfsHookFactory::SwordFsSetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct stat* attr, int to_set,
                                    struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, attr, to_set, fi] {
        vfs->Setattr(req, ino, attr, to_set, fi);
      });
}

void VfsHookFactory::SwordFsReadlink(fuse_req_t req, fuse_ino_t ino) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino] { vfs->Readlink(req, ino); });
}

void VfsHookFactory::SwordFsMknod(fuse_req_t req, fuse_ino_t parent,
                                  const char* name, mode_t mode,
                                  dev_t rdev) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode, rdev] {
        vfs->Mknod(req, parent, name.c_str(), mode, rdev);
      });
}

void VfsHookFactory::SwordFsMkdir(fuse_req_t req, fuse_ino_t parent,
                                  const char* name, mode_t mode) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode] {
        vfs->Mkdir(req, parent, name.c_str(), mode);
      });
}

void VfsHookFactory::SwordFsUnlink(fuse_req_t req, fuse_ino_t parent,
                                   const char* name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->Unlink(req, parent, name.c_str());
      });
}

void VfsHookFactory::SwordFsRmdir(fuse_req_t req, fuse_ino_t parent,
                                  const char* name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->Rmdir(req, parent, name.c_str());
      });
}

void VfsHookFactory::SwordFsSymlink(fuse_req_t req, const char* link,
                                    fuse_ino_t parent, const char* name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, link = std::string(link), parent,
       name = std::string(name)] {
        vfs->Symlink(req, link.c_str(), parent, name.c_str());
      });
}

void VfsHookFactory::SwordFsRename(fuse_req_t req, fuse_ino_t parent,
                                   const char* name, fuse_ino_t newparent,
                                   const char* newname, unsigned int flags) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), newparent,
       newname = std::string(newname), flags] {
        vfs->Rename(req, parent, name.c_str(), newparent, newname.c_str(),
                    flags);
      });
}

void VfsHookFactory::SwordFsLink(fuse_req_t req, fuse_ino_t ino,
                                 fuse_ino_t newparent, const char* newname) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, newparent, newname = std::string(newname)] {
        vfs->Link(req, ino, newparent, newname.c_str());
      });
}

void VfsHookFactory::SwordFsOpen(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Open(req, ino, fi); });
}

void VfsHookFactory::SwordFsRead(fuse_req_t req, fuse_ino_t ino, size_t size,
                                 off_t off, struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size, off, fh] { vfs->Read(req, ino, size, off, fh); });
}

void VfsHookFactory::SwordFsWrite(fuse_req_t req, fuse_ino_t ino,
                                  const char* buf, size_t size, off_t off,
                                  struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, buf = std::string(buf, size), size, off, fh] {
        vfs->Write(req, ino, buf.data(), size, off, fh);
      });
}

void VfsHookFactory::SwordFsFlush(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fh] { vfs->Flush(req, ino, fh); });
}

void VfsHookFactory::SwordFsRelease(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fh] { vfs->Release(req, ino, fh); });
}

void VfsHookFactory::SwordFsFsync(fuse_req_t req, fuse_ino_t ino,
                                  int datasync, struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, datasync, fh] { vfs->Fsync(req, ino, datasync, fh); });
}

void VfsHookFactory::SwordFsOpendir(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Opendir(req, ino, fi); });
}

void VfsHookFactory::SwordFsReaddir(fuse_req_t req, fuse_ino_t ino,
                                    size_t size, off_t off,
                                    struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size, off, fi] { vfs->Readdir(req, ino, size, off, fi); });
}

void VfsHookFactory::SwordFsReleasedir(fuse_req_t req, fuse_ino_t ino,
                                       struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Releasedir(req, ino, fi); });
}

void VfsHookFactory::SwordFsFsyncdir(fuse_req_t req, fuse_ino_t ino,
                                     int datasync,
                                     struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, datasync, fi] { vfs->Fsyncdir(req, ino, datasync, fi); });
}

void VfsHookFactory::SwordFsStatfs(fuse_req_t req, fuse_ino_t ino) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino] { vfs->Statfs(req, ino); });
}

void VfsHookFactory::SwordFsSetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char* name, const char* value,
                                     size_t size, int flags) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name),
       value = std::string(value, size), size, flags] {
        vfs->Setxattr(req, ino, name.c_str(), value.data(), size, flags);
      });
}

void VfsHookFactory::SwordFsGetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char* name, size_t size) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name), size] {
        vfs->Getxattr(req, ino, name.c_str(), size);
      });
}

void VfsHookFactory::SwordFsListxattr(fuse_req_t req, fuse_ino_t ino,
                                      size_t size) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size] { vfs->Listxattr(req, ino, size); });
}

void VfsHookFactory::SwordFsRemovexattr(fuse_req_t req, fuse_ino_t ino,
                                        const char* name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name)] {
        vfs->Removexattr(req, ino, name.c_str());
      });
}

void VfsHookFactory::SwordFsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, mask] { vfs->Access(req, ino, mask); });
}

void VfsHookFactory::SwordFsCreate(fuse_req_t req, fuse_ino_t parent,
                                   const char* name, mode_t mode,
                                   struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode, fi] {
        vfs->Create(req, parent, name.c_str(), mode, fi);
      });
}

void VfsHookFactory::SwordFsGetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi,
                                  struct flock* lock) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)fi;
  (void)lock;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsSetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi,
                                  struct flock* lock, int sleep) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)fi;
  (void)lock;
  (void)sleep;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsBmap(fuse_req_t req, fuse_ino_t ino,
                                 size_t blocksize, uint64_t idx) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)blocksize;
  (void)idx;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsIoctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd,
                                  void* arg, struct fuse_file_info* fi,
                                  unsigned flags, const void* in_buf,
                                  size_t in_bufsz, size_t out_bufsz) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  std::string in_buf_str;
  if (in_buf && in_bufsz > 0) {
    in_buf_str.assign(static_cast<const char*>(in_buf), in_bufsz);
  }
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, cmd, arg, fi, flags,
       in_buf_str = std::move(in_buf_str), in_bufsz, out_bufsz] {
        vfs->Ioctl(req, ino, cmd, arg, fi, flags,
                   in_buf_str.empty() ? nullptr : in_buf_str.data(),
                   in_bufsz, out_bufsz);
      });
}

void VfsHookFactory::SwordFsPoll(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info* fi,
                                 struct fuse_pollhandle* ph) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)fi;
  (void)ph;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsWriteBuf(fuse_req_t req, fuse_ino_t ino,
                                     struct fuse_bufvec* bufv, off_t off,
                                     struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)bufv;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsRetrieveReply(fuse_req_t req, void* cookie,
                                          fuse_ino_t ino, off_t offset,
                                          struct fuse_bufvec* bufv) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsForgetMulti(fuse_req_t req, size_t count,
                                        struct fuse_forget_data* forgets) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  std::vector<fuse_forget_data> forgets_copy(forgets, forgets + count);
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, count,
       forgets_copy = std::move(forgets_copy)]() mutable {
        vfs->ForgetMulti(req, count, forgets_copy.data());
      });
}

void VfsHookFactory::SwordFsFlock(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi, int op) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi, op] { vfs->Flock(req, ino, fi, op); });
}

void VfsHookFactory::SwordFsFallocate(fuse_req_t req, fuse_ino_t ino,
                                      int mode, off_t offset, off_t length,
                                      struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, mode, offset, length, fi] {
        vfs->Fallocate(req, ino, mode, offset, length, fi);
      });
}

void VfsHookFactory::SwordFsReaddirplus(fuse_req_t req, fuse_ino_t ino,
                                        size_t size, off_t off,
                                        struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size, off, fi] { vfs->Readdirplus(req, ino, size, off, fi); });
}

void VfsHookFactory::SwordFsCopyFileRange(
    fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
    struct fuse_file_info* fi_in, fuse_ino_t ino_out, off_t off_out,
    struct fuse_file_info* fi_out, size_t len, int flags) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
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
                                  int whence, struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, off, whence, fi] { vfs->Lseek(req, ino, off, whence, fi); });
}

void VfsHookFactory::SwordFsTmpfile(fuse_req_t req, fuse_ino_t parent,
                                    mode_t mode, struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, mode, fi] { vfs->Tmpfile(req, parent, mode, fi); });
}

void VfsHookFactory::SwordFsStatx(fuse_req_t req, fuse_ino_t ino, int flags,
                                  int mask, struct fuse_file_info* fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [v = vfs_, req, ino, flags, mask, fi] { v->Statx(req, ino, flags, mask, fi); });
}

// Operation table

const struct fuse_lowlevel_ops& VfsHookFactory::get_ops() {
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
