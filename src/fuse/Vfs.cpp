// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE hook factory — static callbacks that forward to SwordfsInterface.

#include "fuse/Vfs.hpp"

#include <string>
#include <vector>

#include "fuse/VfsImpl.hpp"
#include "utils/FiberRuntime.hpp"

namespace swordfs::fuse {

VfsImpl* VfsHookFactory::vfs_ = new VfsImpl();

// ======================================================================
// FUSE callbacks: forward to `VfsImpl`.
// ======================================================================

void VfsHookFactory::SwordfsInit(void* userdata,
                                 struct fuse_conn_info* conn) {
  // Initialise per-thread fiber runtime.
  ::swordfs::utils::InitFiberRuntime();
  vfs_->Init(userdata, conn);
}

void VfsHookFactory::SwordfsDestroy(void* userdata) {
  vfs_->Destroy(userdata);
  // Tear down per-thread fiber runtime.
  ::swordfs::utils::ShutdownFiberRuntime();
}

void VfsHookFactory::SwordfsLookup(fuse_req_t req, fuse_ino_t parent,
                                   const char* name) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->Lookup(req, parent, name.c_str());
      });
}

void VfsHookFactory::SwordfsForget(fuse_req_t req, fuse_ino_t ino,
                                   uint64_t nlookup) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, nlookup] { vfs->Forget(req, ino, nlookup); });
}

void VfsHookFactory::SwordfsGetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Getattr(req, ino, fi); });
}

void VfsHookFactory::SwordfsSetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct stat* attr, int to_set,
                                    struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, attr, to_set, fi] {
        vfs->Setattr(req, ino, attr, to_set, fi);
      });
}

void VfsHookFactory::SwordfsReadlink(fuse_req_t req, fuse_ino_t ino) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino] { vfs->Readlink(req, ino); });
}

void VfsHookFactory::SwordfsMknod(fuse_req_t req, fuse_ino_t parent,
                                  const char* name, mode_t mode,
                                  dev_t rdev) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode, rdev] {
        vfs->Mknod(req, parent, name.c_str(), mode, rdev);
      });
}

void VfsHookFactory::SwordfsMkdir(fuse_req_t req, fuse_ino_t parent,
                                  const char* name, mode_t mode) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode] {
        vfs->Mkdir(req, parent, name.c_str(), mode);
      });
}

void VfsHookFactory::SwordfsUnlink(fuse_req_t req, fuse_ino_t parent,
                                   const char* name) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->Unlink(req, parent, name.c_str());
      });
}

void VfsHookFactory::SwordfsRmdir(fuse_req_t req, fuse_ino_t parent,
                                  const char* name) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->Rmdir(req, parent, name.c_str());
      });
}

void VfsHookFactory::SwordfsSymlink(fuse_req_t req, const char* link,
                                    fuse_ino_t parent, const char* name) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, link = std::string(link), parent,
       name = std::string(name)] {
        vfs->Symlink(req, link.c_str(), parent, name.c_str());
      });
}

void VfsHookFactory::SwordfsRename(fuse_req_t req, fuse_ino_t parent,
                                   const char* name, fuse_ino_t newparent,
                                   const char* newname, unsigned int flags) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), newparent,
       newname = std::string(newname), flags] {
        vfs->Rename(req, parent, name.c_str(), newparent, newname.c_str(),
                    flags);
      });
}

void VfsHookFactory::SwordfsLink(fuse_req_t req, fuse_ino_t ino,
                                 fuse_ino_t newparent, const char* newname) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, newparent, newname = std::string(newname)] {
        vfs->Link(req, ino, newparent, newname.c_str());
      });
}

void VfsHookFactory::SwordfsOpen(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Open(req, ino, fi); });
}

void VfsHookFactory::SwordfsRead(fuse_req_t req, fuse_ino_t ino, size_t size,
                                 off_t off, struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size, off, fi] { vfs->Read(req, ino, size, off, fi); });
}

void VfsHookFactory::SwordfsWrite(fuse_req_t req, fuse_ino_t ino,
                                  const char* buf, size_t size, off_t off,
                                  struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, buf = std::string(buf, size), size, off, fi] {
        vfs->Write(req, ino, buf.data(), size, off, fi);
      });
}

void VfsHookFactory::SwordfsFlush(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Flush(req, ino, fi); });
}

void VfsHookFactory::SwordfsRelease(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Release(req, ino, fi); });
}

void VfsHookFactory::SwordfsFsync(fuse_req_t req, fuse_ino_t ino,
                                  int datasync, struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, datasync, fi] { vfs->Fsync(req, ino, datasync, fi); });
}

void VfsHookFactory::SwordfsOpendir(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Opendir(req, ino, fi); });
}

void VfsHookFactory::SwordfsReaddir(fuse_req_t req, fuse_ino_t ino,
                                    size_t size, off_t off,
                                    struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size, off, fi] { vfs->Readdir(req, ino, size, off, fi); });
}

void VfsHookFactory::SwordfsReleasedir(fuse_req_t req, fuse_ino_t ino,
                                       struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi] { vfs->Releasedir(req, ino, fi); });
}

void VfsHookFactory::SwordfsFsyncdir(fuse_req_t req, fuse_ino_t ino,
                                     int datasync,
                                     struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, datasync, fi] { vfs->Fsyncdir(req, ino, datasync, fi); });
}

void VfsHookFactory::SwordfsStatfs(fuse_req_t req, fuse_ino_t ino) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino] { vfs->Statfs(req, ino); });
}

void VfsHookFactory::SwordfsSetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char* name, const char* value,
                                     size_t size, int flags) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name),
       value = std::string(value, size), size, flags] {
        vfs->Setxattr(req, ino, name.c_str(), value.data(), size, flags);
      });
}

void VfsHookFactory::SwordfsGetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char* name, size_t size) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name), size] {
        vfs->Getxattr(req, ino, name.c_str(), size);
      });
}

void VfsHookFactory::SwordfsListxattr(fuse_req_t req, fuse_ino_t ino,
                                      size_t size) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size] { vfs->Listxattr(req, ino, size); });
}

void VfsHookFactory::SwordfsRemovexattr(fuse_req_t req, fuse_ino_t ino,
                                        const char* name) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name)] {
        vfs->Removexattr(req, ino, name.c_str());
      });
}

void VfsHookFactory::SwordfsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, mask] { vfs->Access(req, ino, mask); });
}

void VfsHookFactory::SwordfsCreate(fuse_req_t req, fuse_ino_t parent,
                                   const char* name, mode_t mode,
                                   struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode, fi] {
        vfs->Create(req, parent, name.c_str(), mode, fi);
      });
}

void VfsHookFactory::SwordfsGetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi,
                                  struct flock* lock) {
  (void)ino;
  (void)fi;
  (void)lock;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordfsSetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi,
                                  struct flock* lock, int sleep) {
  (void)ino;
  (void)fi;
  (void)lock;
  (void)sleep;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordfsBmap(fuse_req_t req, fuse_ino_t ino,
                                 size_t blocksize, uint64_t idx) {
  (void)ino;
  (void)blocksize;
  (void)idx;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordfsIoctl(fuse_req_t req, fuse_ino_t ino, unsigned int cmd,
                                  void* arg, struct fuse_file_info* fi,
                                  unsigned flags, const void* in_buf,
                                  size_t in_bufsz, size_t out_bufsz) {
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

void VfsHookFactory::SwordfsPoll(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info* fi,
                                 struct fuse_pollhandle* ph) {
  (void)ino;
  (void)fi;
  (void)ph;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordfsWriteBuf(fuse_req_t req, fuse_ino_t ino,
                                     struct fuse_bufvec* bufv, off_t off,
                                     struct fuse_file_info* fi) {
  (void)ino;
  (void)bufv;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordfsRetrieveReply(fuse_req_t req, void* cookie,
                                          fuse_ino_t ino, off_t offset,
                                          struct fuse_bufvec* bufv) {
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordfsForgetMulti(fuse_req_t req, size_t count,
                                        struct fuse_forget_data* forgets) {
  std::vector<fuse_forget_data> forgets_copy(forgets, forgets + count);
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, count,
       forgets_copy = std::move(forgets_copy)]() mutable {
        vfs->ForgetMulti(req, count, forgets_copy.data());
      });
}

void VfsHookFactory::SwordfsFlock(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info* fi, int op) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, fi, op] { vfs->Flock(req, ino, fi, op); });
}

void VfsHookFactory::SwordfsFallocate(fuse_req_t req, fuse_ino_t ino,
                                      int mode, off_t offset, off_t length,
                                      struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, mode, offset, length, fi] {
        vfs->Fallocate(req, ino, mode, offset, length, fi);
      });
}

void VfsHookFactory::SwordfsReaddirplus(fuse_req_t req, fuse_ino_t ino,
                                        size_t size, off_t off,
                                        struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, size, off, fi] { vfs->Readdirplus(req, ino, size, off, fi); });
}

void VfsHookFactory::SwordfsCopyFileRange(
    fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
    struct fuse_file_info* fi_in, fuse_ino_t ino_out, off_t off_out,
    struct fuse_file_info* fi_out, size_t len, int flags) {
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

void VfsHookFactory::SwordfsLseek(fuse_req_t req, fuse_ino_t ino, off_t off,
                                  int whence, struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, off, whence, fi] { vfs->Lseek(req, ino, off, whence, fi); });
}

void VfsHookFactory::SwordfsTmpfile(fuse_req_t req, fuse_ino_t parent,
                                    mode_t mode, struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, mode, fi] { vfs->Tmpfile(req, parent, mode, fi); });
}

void VfsHookFactory::SwordfsStatx(fuse_req_t req, fuse_ino_t ino, int flags,
                                  int mask, struct fuse_file_info* fi) {
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, flags, mask, fi] { vfs->Statx(req, ino, flags, mask, fi); });
}

// Operation table

const struct fuse_lowlevel_ops& VfsHookFactory::GetOps() {
  static const struct fuse_lowlevel_ops kOps = {
      .init = SwordfsInit,
      .destroy = SwordfsDestroy,
      .lookup = SwordfsLookup,
      .forget = SwordfsForget,
      .getattr = SwordfsGetattr,
      .setattr = SwordfsSetattr,
      .readlink = SwordfsReadlink,
      .mknod = SwordfsMknod,
      .mkdir = SwordfsMkdir,
      .unlink = SwordfsUnlink,
      .rmdir = SwordfsRmdir,
      .symlink = SwordfsSymlink,
      .rename = SwordfsRename,
      .link = SwordfsLink,
      .open = SwordfsOpen,
      .read = SwordfsRead,
      .write = SwordfsWrite,
      .flush = SwordfsFlush,
      .release = SwordfsRelease,
      .fsync = SwordfsFsync,
      .opendir = SwordfsOpendir,
      .readdir = SwordfsReaddir,
      .releasedir = SwordfsReleasedir,
      .fsyncdir = SwordfsFsyncdir,
      .statfs = SwordfsStatfs,
      .setxattr = SwordfsSetxattr,
      .getxattr = SwordfsGetxattr,
      .listxattr = SwordfsListxattr,
      .removexattr = SwordfsRemovexattr,
      .access = SwordfsAccess,
      .create = SwordfsCreate,
      .getlk = SwordfsGetlk,
      .setlk = SwordfsSetlk,
      .bmap = SwordfsBmap,
      .ioctl = SwordfsIoctl,
      .poll = SwordfsPoll,
      .write_buf = SwordfsWriteBuf,
      .retrieve_reply = SwordfsRetrieveReply,
      .forget_multi = SwordfsForgetMulti,
      .flock = SwordfsFlock,
      .fallocate = SwordfsFallocate,
      .readdirplus = SwordfsReaddirplus,
      .copy_file_range = SwordfsCopyFileRange,
      .lseek = SwordfsLseek,
      .tmpfile = SwordfsTmpfile,
      .statx = SwordfsStatx,
  };
  return kOps;
}

}  // namespace swordfs::fuse
