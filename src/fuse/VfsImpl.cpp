// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "fuse/VfsImpl.hpp"

#include "fuse/Limits.hpp"
#include "metadata/Meta.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"
#include <folly/fibers/FiberManagerInternal.h>

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace swordfs::utils;

using swordfs::metadata::InodeID;
using swordfs::metadata::SwordFsEntry;

namespace swordfs::fuse {

VfsImpl::VfsImpl() {
  if (ConfigCenter::Instance().vfs_backend() == VfsBackend::kMemory) {
    meta_ = std::make_unique<swordfs::metadata::MemMetaImpl>();
  } else {
    SWORDFS_PROMPT_EXIT << "VFS backend not supported";
    return;
  }
}

void VfsImpl::Init(void* userdata, struct fuse_conn_info* conn) {
  (void)userdata;
  conn->no_interrupt = 1;
  conn->max_write = kMaxWriteSize;
  conn->max_readahead = kMaxReadAheadSize;
  conn->time_gran = kTimeGran;
  conn->want |= FUSE_CAP_WRITEBACK_CACHE;
  conn->want |= FUSE_CAP_SPLICE_WRITE;
  conn->want |= FUSE_CAP_SPLICE_READ;
  conn->want |= FUSE_CAP_READDIRPLUS;
  conn->want |= FUSE_CAP_ASYNC_READ;
  conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;
  conn->want |= FUSE_CAP_DONT_MASK;
  SWORDFS_LOG_INFO << "SwordFS filesystem initialized (mount OK)";
}

void VfsImpl::Destroy(void* userdata) {
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
}

void VfsImpl::Lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  InodeID child_ino;
  struct stat attr;
  Status status = meta_->Lookup(parent, name, &child_ino,
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
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  meta_->Forget(ino, nlookup);
}

void VfsImpl::Getattr(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  (void)fi;
  struct stat attr;
  Status status = meta_->GetAttr(ino, &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    fuse_reply_attr(req, &attr, 1.0);
  }
}

void VfsImpl::Setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                      int to_set, struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  struct stat out_attr;
  Status status = meta_->SetAttr(ino, attr, to_set,
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

void VfsImpl::Mknod(fuse_req_t req, fuse_ino_t parent, const char* name,
                    mode_t mode, dev_t rdev) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)rdev;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Mkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
                    mode_t mode) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  InodeID child_ino;
  struct stat attr;
  Status status = meta_->MkDir(parent, name, mode,
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

void VfsImpl::Unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_->Unlink(parent, name);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_->RmDir(parent, name);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Symlink(fuse_req_t req, const char* link, fuse_ino_t parent,
                      const char* name) {
  (void)link;
  (void)parent;
  (void)name;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Rename(fuse_req_t req, fuse_ino_t parent, const char* name,
                     fuse_ino_t newparent, const char* newname,
                     unsigned int flags) {
  (void)flags;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_->Rename(parent, name, newparent,
                                  newname);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                   const char* newname) {
  (void)ino;
  (void)newparent;
  (void)newname;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Open(fuse_req_t req, fuse_ino_t ino,
                   struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  uint64_t fh;
  Status status = meta_->Open(ino, &fh);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fi->fh = fh;
  fuse_reply_open(req, fi);
}

void VfsImpl::Read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                   struct fuse_file_info* fi) {
  (void)ino;
  (void)size;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Write(fuse_req_t req, fuse_ino_t ino, const char* buf,
                    size_t size, off_t off, struct fuse_file_info* fi) {
  (void)ino;
  (void)buf;
  (void)size;
  (void)off;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Flush(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info* fi) {
  (void)ino;
  (void)fi;
  fuse_reply_err(req, 0);
}

void VfsImpl::Release(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_->Release(fi->fh);
  (void)ino;
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                    struct fuse_file_info* fi) {
  (void)ino;
  (void)datasync;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Opendir(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  uint64_t fh;
  Status status = meta_->OpenDir(ino, &fh);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fi->fh = fh;
  fuse_reply_open(req, fi);
}

void VfsImpl::Readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                      off_t off, struct fuse_file_info* fi) {
  (void)fi;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  std::vector<swordfs::metadata::SwordFsEntry> entries;
  Status status = meta_->ReadDir(ino, &entries);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  // Build dirent buffer
  size_t cap = 0;
  for (const auto& e : entries) {
    cap += fuse_add_direntry(req, nullptr, 0, e.name.c_str(), nullptr, 0);
  }
  char* buf = static_cast<char*>(std::malloc(cap));
  if (!buf) {
    fuse_reply_err(req, ENOMEM);
    return;
  }
  size_t pos = 0;
  struct stat stbuf;
  for (const auto& e : entries) {
    std::memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = e.ino;
    stbuf.st_mode = e.type << 12;
    pos += fuse_add_direntry(req, buf + pos, cap - pos, e.name.c_str(),
                             &stbuf, cap);
  }
  if (static_cast<size_t>(off) < pos) {
    size_t remain = pos - static_cast<size_t>(off);
    size_t reply_sz = remain < size ? remain : size;
    fuse_reply_buf(req, buf + static_cast<size_t>(off), reply_sz);
  } else {
    fuse_reply_buf(req, nullptr, 0);
  }
  std::free(buf);
}

void VfsImpl::Releasedir(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_->ReleaseDir(fi->fh);
  (void)ino;
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                       struct fuse_file_info* fi) {
  (void)ino;
  (void)datasync;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Statfs(fuse_req_t req, fuse_ino_t ino) {
  (void)ino;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  struct statvfs stbuf;
  Status status = meta_->StatFs(&stbuf);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    fuse_reply_statfs(req, &stbuf);
  }
}

void VfsImpl::Setxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                       const char* value, size_t size, int flags) {
  (void)ino;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Getxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
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

void VfsImpl::Removexattr(fuse_req_t req, fuse_ino_t ino, const char* name) {
  (void)ino;
  (void)name;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Access(fuse_req_t req, fuse_ino_t ino, int mask) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_->Access(ino, mask);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Create(fuse_req_t req, fuse_ino_t parent, const char* name,
                     mode_t mode, struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  InodeID child_ino;
  struct stat attr;
  Status status = meta_->Create(parent, name, mode, &child_ino, &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  uint64_t fh;
  Status status2 = meta_->Open(child_ino, &fh);
  if (!status2.ok()) {
    fuse_reply_err(req, status2.ToErrno());
    return;
  }
  fi->fh = fh;
  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  fuse_reply_create(req, &entry, fi);
}

void VfsImpl::Ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void* arg,
                    struct fuse_file_info* fi, unsigned flags,
                    const void* in_buf, size_t in_bufsz, size_t out_bufsz) {
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

void VfsImpl::RetrieveReply(fuse_req_t req, void* cookie, fuse_ino_t ino,
                            off_t offset, struct fuse_bufvec* bufv) {
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::ForgetMulti(fuse_req_t req, size_t count,
                          struct fuse_forget_data* forgets) {
  (void)count;
  (void)forgets;
  fuse_reply_none(req);
}

void VfsImpl::Flock(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info* fi, int op) {
  (void)ino;
  (void)fi;
  (void)op;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
                        off_t offset, off_t length,
                        struct fuse_file_info* fi) {
  (void)ino;
  (void)mode;
  (void)offset;
  (void)length;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info* fi) {
  (void)fi;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  SWORDFS_LOG_DEBUG << "Readdirplus: ino=" << ino << " size=" << size << " off=" << off;
  std::vector<swordfs::metadata::SwordFsEntry> entries;
  Status status = meta_->ReadDir(ino, &entries);
  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Readdirplus failed: ino=" << ino << " errno=" << status.ToErrno();
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  SWORDFS_LOG_DEBUG << "Readdirplus: got " << entries.size() << " entries";

  // First pass: calculate total buffer size and per-entry sizes
  std::vector<size_t> entry_sizes;
  entry_sizes.reserve(entries.size());
  size_t cap = 0;
  for (const auto& e : entries) {
    struct fuse_entry_param ep = {};
    size_t sz = fuse_add_direntry_plus(req, nullptr, 0, e.name.c_str(), &ep, 0);
    entry_sizes.push_back(sz);
    cap += sz;
  }
  char* buf = static_cast<char*>(std::malloc(cap));
  if (!buf) {
    fuse_reply_err(req, ENOMEM);
    return;
  }

  // Second pass: fill buffer with full attrs
  size_t pos = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    const auto& e = entries[i];
    struct stat attr;
    Status status = meta_->GetAttr(e.ino, &attr);
    if (!status.ok()) {
      std::memset(&attr, 0, sizeof(attr));
    }
    attr.st_ino = e.ino;
    attr.st_mode = e.type << 12;

    struct fuse_entry_param ep = {};
    ep.ino = e.ino;
    ep.attr = attr;
    ep.attr_timeout = 1.0;
    ep.entry_timeout = 1.0;

    size_t entsize = fuse_add_direntry_plus(req, buf + pos, cap - pos,
                                            e.name.c_str(), &ep,
                                            pos + entry_sizes[i]);
    if (entsize > cap - pos) break;
    pos += entsize;
  }

  if (static_cast<size_t>(off) < pos) {
    size_t remain = pos - static_cast<size_t>(off);
    size_t reply_sz = remain < size ? remain : size;
    fuse_reply_buf(req, buf + static_cast<size_t>(off), reply_sz);
  } else {
    fuse_reply_buf(req, nullptr, 0);
  }
  std::free(buf);
}

void VfsImpl::Lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
                    struct fuse_file_info* fi) {
  (void)ino;
  (void)off;
  (void)whence;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Tmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
                      struct fuse_file_info* fi) {
  (void)parent;
  (void)mode;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Statx(fuse_req_t req, fuse_ino_t ino, int flags, int mask,
                    struct fuse_file_info* fi) {
  (void)ino;
  (void)flags;
  (void)mask;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

}  // namespace swordfs::fuse
