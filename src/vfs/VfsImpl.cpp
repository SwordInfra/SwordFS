// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/VfsImpl.hpp"

#include <dirent.h>
#include <folly/fibers/FiberManager.h>

#include "config/ConfigCenter.hpp"
#include "fuse/Limits.hpp"
#include "metadata/Meta.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandleManager.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace swordfs::utils;
using namespace swordfs::config;

using swordfs::metadata::InodeID;
using swordfs::metadata::SwordFsEntry;

namespace swordfs::vfs {

VfsImpl::VfsImpl() = default;

VfsImpl::~VfsImpl() = default;

void VfsImpl::Init(std::unique_ptr<volume::VolumeImpl> vol) {
  vol_ = std::move(vol);
}

void VfsImpl::SetRequestContext(fuse_req_t req) {
  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx = SwordFsContext{fuse_req_ctx(req)};
  ctx.vol = vol_.get();
}

void VfsImpl::Lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  SetRequestContext(req);
  InodeID child_ino;
  struct stat attr;
  Status status = vol_->meta_engine()->Lookup(parent, name, &child_ino,
                                              &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  fuse_reply_entry(req, &entry);
}

void VfsImpl::Forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
  SetRequestContext(req);
  vol_->meta_engine()->Forget(ino, nlookup);
}

void VfsImpl::Getattr(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info *fi) {
  SetRequestContext(req);
  (void)fi;
  struct stat attr;
  Status status = vol_->meta_engine()->GetAttr(ino, &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    fuse_reply_attr(req, &attr, 1.0);
  }
}

void VfsImpl::Setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                      int to_set, struct fuse_file_info *fi) {
  SetRequestContext(req);
  struct stat out_attr;
  Status status = vol_->meta_engine()->SetAttr(ino, attr, to_set,
                                               &out_attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    (void)fi;
    fuse_reply_attr(req, &out_attr, 1.0);
  }
}

void VfsImpl::Readlink(fuse_req_t req, fuse_ino_t ino) {
  (void)ino;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
                    mode_t mode, dev_t rdev) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)rdev;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                    mode_t mode) {
  SetRequestContext(req);
  InodeID child_ino;
  struct stat attr;
  Status status = vol_->meta_engine()->MkDir(parent, name, mode,
                                             &child_ino, &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  fuse_reply_entry(req, &entry);
}

void VfsImpl::Unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
  SetRequestContext(req);
  Status status = vol_->meta_engine()->Unlink(parent, name);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
  SetRequestContext(req);
  Status status = vol_->meta_engine()->RmDir(parent, name);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
                      const char *name) {
  (void)link;
  (void)parent;
  (void)name;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                     fuse_ino_t newparent, const char *newname,
                     unsigned int flags) {
  SetRequestContext(req);
  Status status = vol_->meta_engine()->Rename(parent, name, newparent,
                                              newname, flags);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                   const char *newname) {
  (void)ino;
  (void)newparent;
  (void)newname;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Open(fuse_req_t req, fuse_ino_t ino,
                   struct fuse_file_info *fi) {
  SetRequestContext(req);
  uint64_t fh;
  Status status = vol_->meta_engine()->Open(ino, &fh);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  } else {
    SWORDFS_LOG_DEBUG << "Open: ino=" << ino << " fh=" << fh;
    fi->fh = fh;
  }
  status = FileHandleManager::Instance().Open(fh, vol_.get(), ino);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " fh=" << fh
                      << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fuse_reply_open(req, fi);
}

void VfsImpl::Read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                   uint64_t fh) {
  SetRequestContext(req);
  (void)fh;

  auto buf = folly::IOBuf::create(size);
  auto rw = FileHandleManager::Instance().Find(fh);
  if (!rw) {
    SWORDFS_LOG_ERROR << "Read: no handle for fh=" << fh;
    fuse_reply_err(req, EBADF);
    return;
  }
  Status status = rw->Read(size, off, buf.get());
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Read failed: ino=" << ino << " offset=" << off
                      << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }

  SWORDFS_LOG_DEBUG << "Read: ino=" << ino << " offset=" << off
                    << " nread=" << buf->length();
  fuse_reply_buf(req, reinterpret_cast<const char *>(buf->data()),
                 buf->length());
}

void VfsImpl::Write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                    size_t size, off_t off, uint64_t fh) {
  SetRequestContext(req);
  SWORDFS_LOG_DEBUG << "Write: ino=" << ino << " fh=" << fh
                    << " size=" << size << " off=" << off;

  auto rw = FileHandleManager::Instance().Find(fh);
  if (!rw) {
    SWORDFS_LOG_ERROR << "Write: no handle for fh=" << fh
                      << " ino=" << ino;
    fuse_reply_err(req, EBADF);
    return;
  }
  Status status = rw->Write(buf, size, off);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Write failed: ino=" << ino << " offset=" << off
                      << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }

  fuse_reply_write(req, size);
}

void VfsImpl::Flush(fuse_req_t req, fuse_ino_t ino,
                    uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Flush: ino=" << ino << " fh=" << fh;
  auto rw = FileHandleManager::Instance().Find(fh);
  Status status = rw ? rw->Flush() : Status::OK();
  fuse_reply_err(req, status.ok() ? 0 : status.ToErrno());
}

void VfsImpl::Release(fuse_req_t req, fuse_ino_t ino,
                      uint64_t fh) {
  SetRequestContext(req);

  // Flush any remaining buffered data before releasing.
  FileHandleManager::Instance().Release(fh);

  Status status = vol_->meta_engine()->Release(fh);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Release FAILED: ino=" << ino << " fh=" << fh
                      << " — " << status.message();
  } else {
    SWORDFS_LOG_DEBUG << "Release: ino=" << ino << " fh=" << fh;
  }
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                    uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Fsync: ino=" << ino << " datasync=" << datasync
                    << " fh=" << fh;
  auto rw = FileHandleManager::Instance().Find(fh);
  Status status = rw ? rw->Flush() : Status::OK();
  fuse_reply_err(req, status.ok() ? 0 : status.ToErrno());
}

void VfsImpl::Opendir(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info *fi) {
  SetRequestContext(req);
  uint64_t fh;
  Status status = vol_->meta_engine()->OpenDir(ino, &fh);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fi->fh = fh;
  fuse_reply_open(req, fi);
}

// Common implementation for Readdir and Readdirplus.
// 1. Read directory entries + prepend "." / ".."
// 2. Two-pass buffer construction: pass 1 calculates sizes,
//    pass 2 fills the buffer with correct `off` values.
// The `add_entry` callback is the only difference: fuse_add_direntry
// for Readdir, fuse_add_direntry_plus for Readdirplus.
template <typename F>
static void ReaddirCommon(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off,
                          swordfs::metadata::IMetaEngine *meta,
                          F &&add_entry) {
  using swordfs::metadata::SwordFsEntry;

  std::vector<SwordFsEntry> entries;
  Status status = meta->ReadDir(ino, &entries);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }

  // "." and ".." required by FUSE low-level API.
  entries.insert(entries.begin(), {".", DT_DIR, ino});
  entries.insert(entries.begin() + 1,
                 {"..", DT_DIR, (ino == FUSE_ROOT_ID) ? ino : 0});

  std::vector<size_t> sizes(entries.size());
  size_t cap = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    sizes[i] = add_entry(req, nullptr, 0, entries[i], 0);
    cap += sizes[i];
  }

  char *buf = static_cast<char *>(std::malloc(cap));
  if (!buf) {
    fuse_reply_err(req, ENOMEM);
    return;
  }

  size_t pos = 0;
  for (size_t i = 0; i < entries.size() && pos < cap; ++i) {
    size_t n = add_entry(req, buf + pos, cap - pos,
                         entries[i], pos + sizes[i]);
    if (n > cap - pos) break;
    pos += n;
  }

  if (static_cast<size_t>(off) < pos)
    fuse_reply_buf(req, buf + off, std::min(pos - off, size));
  else
    fuse_reply_buf(req, nullptr, 0);
  std::free(buf);
}

void VfsImpl::Readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                      off_t off, struct fuse_file_info *fi) {
  (void)fi;
  SetRequestContext(req);

  ReaddirCommon(req, ino, size, off, vol_->meta_engine(),
                [](fuse_req_t req, char *buf, size_t bufsize,
                   const swordfs::metadata::SwordFsEntry &e, off_t off) {
                  struct stat st = {};
                  st.st_ino = e.ino;
                  st.st_mode = e.type << 12;
                  return fuse_add_direntry(req, buf, bufsize, e.name.c_str(), &st, off);
                });
}

void VfsImpl::Releasedir(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi) {
  SetRequestContext(req);
  Status status = vol_->meta_engine()->ReleaseDir(fi->fh);
  (void)ino;
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                       struct fuse_file_info *fi) {
  (void)ino;
  (void)datasync;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Statfs(fuse_req_t req, fuse_ino_t ino) {
  (void)ino;
  SetRequestContext(req);
  struct statvfs stbuf;
  Status status = vol_->meta_engine()->StatFs(&stbuf);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    fuse_reply_statfs(req, &stbuf);
  }
}

void VfsImpl::Setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                       const char *value, size_t size, int flags) {
  (void)ino;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                       size_t size) {
  (void)ino;
  (void)name;
  (void)size;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  (void)ino;
  (void)size;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
  (void)ino;
  (void)name;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Access(fuse_req_t req, fuse_ino_t ino, int mask) {
  SetRequestContext(req);
  Status status = vol_->meta_engine()->Access(ino, mask);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Create(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode, struct fuse_file_info *fi) {
  SetRequestContext(req);
  InodeID child_ino;
  struct stat attr;
  Status status = vol_->meta_engine()->Create(parent, name, mode, &child_ino, &attr);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create FAILED: parent=" << parent << " name='" << name
                      << "' — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  uint64_t fh;
  status = vol_->meta_engine()->Open(child_ino, &fh);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create: Open FAILED: ino=" << child_ino
                      << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fi->fh = fh;

  status = FileHandleManager::Instance().Open(fh, vol_.get(), child_ino);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create: FileHandleManager::Open FAILED: fh=" << fh
                      << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }

  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  SWORDFS_LOG_INFO << "Create: ino=" << child_ino << " fh=" << fh
                   << " name='" << name << "'";
  fuse_reply_create(req, &entry, fi);
}

void VfsImpl::Ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
                    struct fuse_file_info *fi, unsigned flags,
                    const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
  (void)ino;
  (void)cmd;
  (void)arg;
  (void)fi;
  (void)flags;
  (void)in_buf;
  (void)in_bufsz;
  (void)out_bufsz;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::RetrieveReply(fuse_req_t req, void *cookie, fuse_ino_t ino,
                            off_t offset, struct fuse_bufvec *bufv) {
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::ForgetMulti(fuse_req_t req, size_t count,
                          struct fuse_forget_data *forgets) {
  (void)count;
  (void)forgets;
  fuse_reply_none(req);
}

void VfsImpl::Flock(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info *fi, int op) {
  (void)ino;
  (void)fi;
  (void)op;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
                        off_t offset, off_t length,
                        struct fuse_file_info *fi) {
  (void)ino;
  (void)mode;
  (void)offset;
  (void)length;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi) {
  (void)fi;
  SetRequestContext(req);

  ReaddirCommon(req, ino, size, off, vol_->meta_engine(),
                [this](fuse_req_t req, char *buf, size_t bufsize,
                       const swordfs::metadata::SwordFsEntry &e, off_t off) {
                  struct stat attr = {};
                  if (e.ino != 0) vol_->meta_engine()->GetAttr(e.ino, &attr);
                  struct fuse_entry_param ep = {};
                  ep.ino = e.ino;
                  ep.attr = attr;
                  ep.attr.st_ino = e.ino;
                  ep.attr.st_mode = e.type << 12;
                  ep.attr_timeout = 1.0;
                  ep.entry_timeout = 1.0;
                  return fuse_add_direntry_plus(req, buf, bufsize,
                                                e.name.c_str(), &ep, off);
                });
}

void VfsImpl::Lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
                    struct fuse_file_info *fi) {
  (void)ino;
  (void)off;
  (void)whence;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Tmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
                      struct fuse_file_info *fi) {
  (void)parent;
  (void)mode;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Statx(fuse_req_t req, fuse_ino_t ino, int flags, int mask,
                    struct fuse_file_info *fi) {
  (void)ino;
  (void)flags;
  (void)mask;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

}  // namespace swordfs::vfs
