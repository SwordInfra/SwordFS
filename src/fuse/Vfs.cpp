// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS FUSE hook factory — static callbacks that forward to SwordFsInterface.

#include "fuse/Vfs.hpp"

#include <dirent.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "fuse/Limits.hpp"
#include "metadata/Meta.hpp"
#include "metadata/Types.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::vfs::VfsImpl;

namespace swordfs::fuse {

VfsImpl *VfsHookFactory::vfs_ = new VfsImpl();

void VfsHookFactory::BindVolume(std::unique_ptr<volume::VolumeImpl> vol) {
  vfs_->Init(std::move(vol));
}

// ────────────────────────────────────────────────────────────────
// FUSE callbacks: forward to `VfsImpl`.
// ────────────────────────────────────────────────────────────────

void VfsHookFactory::SwordFsInit(void *userdata,
                                 struct fuse_conn_info *conn) {
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

void VfsHookFactory::SwordFsDestroy(void *userdata) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
  ::swordfs::utils::ShutdownFiberRuntime();
}

void VfsHookFactory::SwordFsLookup(fuse_req_t req, fuse_ino_t parent,
                                   const char *name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->SetRequestContext(req);
        fuse_entry_param entry;
        auto status = vfs->Lookup(parent, name.c_str(), &entry);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_entry(req, &entry);
      });
}

void VfsHookFactory::SwordFsForget(fuse_req_t req, fuse_ino_t ino,
                                   uint64_t nlookup) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, ino, nlookup] { vfs->Forget(ino, nlookup); });
}

void VfsHookFactory::SwordFsGetattr(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)fi;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino] {
    vfs->SetRequestContext(req);
    struct stat attr;
    auto status = vfs->Getattr(ino, &attr);
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
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)fi;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, attr, to_set] {
    vfs->SetRequestContext(req);
    struct stat out_attr;
    auto status = vfs->Setattr(ino, attr, to_set, &out_attr);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_attr(req, &out_attr, 1.0);
  });
}

void VfsHookFactory::SwordFsReadlink(fuse_req_t req, fuse_ino_t ino) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino] {
    vfs->SetRequestContext(req);
    auto status = vfs->Readlink(ino);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsMknod(fuse_req_t req, fuse_ino_t parent,
                                  const char *name, mode_t mode,
                                  dev_t rdev) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode, rdev] {
        vfs->SetRequestContext(req);
        auto status = vfs->Mknod(parent, name.c_str(), mode, rdev);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsMkdir(fuse_req_t req, fuse_ino_t parent,
                                  const char *name, mode_t mode) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode] {
        vfs->SetRequestContext(req);
        fuse_entry_param entry;
        auto status = vfs->Mkdir(parent, name.c_str(), mode, &entry);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_entry(req, &entry);
      });
}

void VfsHookFactory::SwordFsUnlink(fuse_req_t req, fuse_ino_t parent,
                                   const char *name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->SetRequestContext(req);
        auto status = vfs->Unlink(parent, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsRmdir(fuse_req_t req, fuse_ino_t parent,
                                  const char *name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name)] {
        vfs->SetRequestContext(req);
        auto status = vfs->Rmdir(parent, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsSymlink(fuse_req_t req, const char *link,
                                    fuse_ino_t parent, const char *name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, link = std::string(link), parent,
       name = std::string(name)] {
        vfs->SetRequestContext(req);
        auto status = vfs->Symlink(link.c_str(), parent, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsRename(fuse_req_t req, fuse_ino_t parent,
                                   const char *name, fuse_ino_t newparent,
                                   const char *newname, unsigned int flags) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), newparent,
       newname = std::string(newname), flags] {
        vfs->SetRequestContext(req);
        auto status = vfs->Rename(parent, name.c_str(), newparent,
                                  newname.c_str(), flags);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsLink(fuse_req_t req, fuse_ino_t ino,
                                 fuse_ino_t newparent, const char *newname) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, newparent, newname = std::string(newname)] {
        vfs->SetRequestContext(req);
        auto status = vfs->Link(ino, newparent, newname.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsOpen(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, fi] {
    vfs->SetRequestContext(req);
    auto status = vfs->Open(ino, fi);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_open(req, fi);
  });
}

void VfsHookFactory::SwordFsRead(fuse_req_t req, fuse_ino_t ino, size_t size,
                                 off_t off, struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, size, off, fh] {
    vfs->SetRequestContext(req);
    std::unique_ptr<folly::IOBuf> data;
    auto status = vfs->Read(ino, size, off, fh, &data);
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
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino,
       buf = folly::IOBuf::copyBuffer(buf, size), off, fh] {
        vfs->SetRequestContext(req);
        auto status = vfs->Write(ino, *buf, off, fh);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_write(req, buf->length());
      });
}

void VfsHookFactory::SwordFsFlush(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, fh] {
    vfs->SetRequestContext(req);
    auto status = vfs->Flush(ino, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_err(req, 0);
  });
}

void VfsHookFactory::SwordFsRelease(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, fh] {
    vfs->SetRequestContext(req);
    auto status = vfs->Release(ino, fh);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFsync(fuse_req_t req, fuse_ino_t ino,
                                  int datasync, struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  uint64_t fh = fi->fh;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, datasync, fh] {
    vfs->SetRequestContext(req);
    auto status = vfs->Fsync(ino, datasync, fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_err(req, 0);
  });
}

void VfsHookFactory::SwordFsOpendir(fuse_req_t req, fuse_ino_t ino,
                                    struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, fi] {
    vfs->SetRequestContext(req);
    auto status = vfs->Opendir(ino, &fi->fh);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    fuse_reply_open(req, fi);
  });
}

void VfsHookFactory::SwordFsReaddir(fuse_req_t req, fuse_ino_t ino,
                                    size_t size, off_t off,
                                    struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)fi;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, size, off] {
    vfs->SetRequestContext(req);
    std::vector<swordfs::metadata::SwordFsEntry> entries;
    entries.push_back(swordfs::metadata::SwordFsEntry{".", DT_DIR, ino});
    entries.push_back(swordfs::metadata::SwordFsEntry{
        "..", DT_DIR, ino == FUSE_ROOT_ID ? ino : 0});
    auto status = vfs->Volume()->meta_engine()->ReadDir(ino, &entries);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    size_t cap = 0;
    std::vector<size_t> sizes(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      struct stat st = {};
      st.st_ino = entries[i].ino;
      st.st_mode = entries[i].type << 12;
      sizes[i] = fuse_add_direntry(req, nullptr, 0,
                                   entries[i].name.c_str(), &st, 0);
      cap += sizes[i];
    }
    char *buf = static_cast<char *>(std::malloc(cap));
    if (!buf) {
      fuse_reply_err(req, ENOMEM);
      return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < entries.size() && pos < cap; ++i) {
      struct stat st = {};
      st.st_ino = entries[i].ino;
      st.st_mode = entries[i].type << 12;
      size_t n = fuse_add_direntry(req, buf + pos, cap - pos,
                                   entries[i].name.c_str(), &st,
                                   pos + sizes[i]);
      if (n > cap - pos) break;
      pos += n;
    }
    if (static_cast<size_t>(off) < pos)
      fuse_reply_buf(req, buf + off, std::min(pos - off, size));
    else
      fuse_reply_buf(req, nullptr, 0);
    std::free(buf);
  });
}

void VfsHookFactory::SwordFsReleasedir(fuse_req_t req, fuse_ino_t ino,
                                       struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, fi] {
    vfs->SetRequestContext(req);
    auto status = vfs->Releasedir(ino, fi->fh);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFsyncdir(fuse_req_t req, fuse_ino_t ino,
                                     int datasync,
                                     struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)fi;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, datasync] {
    vfs->SetRequestContext(req);
    auto status = vfs->Fsyncdir(ino, datasync);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsStatfs(fuse_req_t req, fuse_ino_t ino) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino] {
    vfs->SetRequestContext(req);
    struct statvfs stbuf;
    auto status = vfs->Statfs(ino, &stbuf);
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
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name),
       value = std::string(value, size), size, flags] {
        vfs->SetRequestContext(req);
        auto status = vfs->Setxattr(ino, name.c_str(), value.data(),
                                    size, flags);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsGetxattr(fuse_req_t req, fuse_ino_t ino,
                                     const char *name, size_t size) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name), size] {
        vfs->SetRequestContext(req);
        auto status = vfs->Getxattr(ino, name.c_str(), size);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsListxattr(fuse_req_t req, fuse_ino_t ino,
                                      size_t size) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, size] {
    vfs->SetRequestContext(req);
    auto status = vfs->Listxattr(ino, size);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsRemovexattr(fuse_req_t req, fuse_ino_t ino,
                                        const char *name) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, name = std::string(name)] {
        vfs->SetRequestContext(req);
        auto status = vfs->Removexattr(ino, name.c_str());
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsAccess(fuse_req_t req, fuse_ino_t ino, int mask) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, mask] {
    vfs->SetRequestContext(req);
    auto status = vfs->Access(ino, mask);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsCreate(fuse_req_t req, fuse_ino_t parent,
                                   const char *name, mode_t mode,
                                   struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, parent, name = std::string(name), mode, fi] {
        vfs->SetRequestContext(req);
        fuse_entry_param entry;
        auto status = vfs->Create(parent, name.c_str(), mode, &entry, fi);
        if (!status.ok()) {
          fuse_reply_err(req, status.ToErrno());
          return;
        }
        fuse_reply_create(req, &entry, fi);
      });
}

void VfsHookFactory::SwordFsGetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi,
                                  struct flock *lock) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)fi;
  (void)lock;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsSetlk(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi,
                                  struct flock *lock, int sleep) {
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
                                  void *arg, struct fuse_file_info *fi,
                                  unsigned flags, const void *in_buf,
                                  size_t in_bufsz, size_t out_bufsz) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  std::string in_buf_str;
  if (in_buf && in_bufsz > 0) {
    in_buf_str.assign(static_cast<const char *>(in_buf), in_bufsz);
  }
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, cmd, arg, fi, flags,
       in_buf_str = std::move(in_buf_str), in_bufsz, out_bufsz] {
        vfs->SetRequestContext(req);
        auto status = vfs->Ioctl(ino, static_cast<int>(cmd), arg, fi, flags,
                                 in_buf_str.empty() ? nullptr : in_buf_str.data(),
                                 in_bufsz, out_bufsz);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsPoll(fuse_req_t req, fuse_ino_t ino,
                                 struct fuse_file_info *fi,
                                 struct fuse_pollhandle *ph) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)fi;
  (void)ph;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsWriteBuf(fuse_req_t req, fuse_ino_t ino,
                                     struct fuse_bufvec *bufv, off_t off,
                                     struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)ino;
  (void)bufv;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsHookFactory::SwordFsRetrieveReply(fuse_req_t req, void *cookie,
                                          fuse_ino_t ino, off_t offset,
                                          struct fuse_bufvec *bufv) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  auto status = vfs_->RetrieveReply(req, cookie, ino, offset, bufv);
  fuse_reply_err(req, status.ToErrno());
}

void VfsHookFactory::SwordFsForgetMulti(fuse_req_t req, size_t count,
                                        struct fuse_forget_data *forgets) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  std::vector<fuse_forget_data> forgets_copy(forgets, forgets + count);
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, count,
       forgets_copy = std::move(forgets_copy)]() mutable {
        vfs->ForgetMulti(req, count, forgets_copy.data());
      });
}

void VfsHookFactory::SwordFsFlock(fuse_req_t req, fuse_ino_t ino,
                                  struct fuse_file_info *fi, int op) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, fi, op] {
    vfs->SetRequestContext(req);
    auto status = vfs->Flock(ino, fi, op);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsFallocate(fuse_req_t req, fuse_ino_t ino,
                                      int mode, off_t offset, off_t length,
                                      struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber(
      [vfs = vfs_, req, ino, mode, offset, length, fi] {
        vfs->SetRequestContext(req);
        auto status = vfs->Fallocate(ino, mode, offset, length, fi);
        fuse_reply_err(req, status.ToErrno());
      });
}

void VfsHookFactory::SwordFsReaddirplus(fuse_req_t req, fuse_ino_t ino,
                                        size_t size, off_t off,
                                        struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  (void)fi;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, size, off] {
    vfs->SetRequestContext(req);
    std::vector<swordfs::metadata::SwordFsEntry> entries;
    entries.push_back(swordfs::metadata::SwordFsEntry{".", DT_DIR, ino});
    entries.push_back(swordfs::metadata::SwordFsEntry{
        "..", DT_DIR, ino == FUSE_ROOT_ID ? ino : 0});
    auto status = vfs->Volume()->meta_engine()->ReadDir(ino, &entries);
    if (!status.ok()) {
      fuse_reply_err(req, status.ToErrno());
      return;
    }
    size_t cap = 0;
    std::vector<size_t> sizes(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      fuse_entry_param e = {};
      e.ino = entries[i].ino;
      e.attr.st_ino = entries[i].ino;
      e.attr.st_mode = entries[i].type << 12;
      e.attr_timeout = 86400;
      e.entry_timeout = 86400;
      sizes[i] = fuse_add_direntry_plus(req, nullptr, 0,
                                        entries[i].name.c_str(), &e, 0);
      cap += sizes[i];
    }
    char *buf = static_cast<char *>(std::malloc(cap));
    if (!buf) {
      fuse_reply_err(req, ENOMEM);
      return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < entries.size() && pos < cap; ++i) {
      fuse_entry_param e = {};
      e.ino = entries[i].ino;
      e.attr.st_ino = entries[i].ino;
      e.attr.st_mode = entries[i].type << 12;
      e.attr_timeout = 86400;
      e.entry_timeout = 86400;
      size_t n = fuse_add_direntry_plus(req, buf + pos, cap - pos,
                                        entries[i].name.c_str(), &e,
                                        pos + sizes[i]);
      if (n > cap - pos) break;
      pos += n;
    }
    if (static_cast<size_t>(off) < pos)
      fuse_reply_buf(req, buf + off, std::min(pos - off, size));
    else
      fuse_reply_buf(req, nullptr, 0);
    std::free(buf);
  });
}

void VfsHookFactory::SwordFsCopyFileRange(
    fuse_req_t req, fuse_ino_t ino_in, off_t off_in,
    struct fuse_file_info *fi_in, fuse_ino_t ino_out, off_t off_out,
    struct fuse_file_info *fi_out, size_t len, int flags) {
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
                                  int whence, struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, off, whence, fi] {
    vfs->SetRequestContext(req);
    auto status = vfs->Lseek(ino, off, whence, fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsTmpfile(fuse_req_t req, fuse_ino_t parent,
                                    mode_t mode, struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, parent, mode, fi] {
    vfs->SetRequestContext(req);
    auto status = vfs->Tmpfile(parent, mode, fi);
    fuse_reply_err(req, status.ToErrno());
  });
}

void VfsHookFactory::SwordFsStatx(fuse_req_t req, fuse_ino_t ino, int flags,
                                  int mask, struct fuse_file_info *fi) {
  SWORDFS_LOG_DEBUG << "FUSE " << __func__;
  ::swordfs::utils::RunInFiber([vfs = vfs_, req, ino, flags, mask, fi] {
    vfs->SetRequestContext(req);
    auto status = vfs->Statx(ino, flags, mask, fi);
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
