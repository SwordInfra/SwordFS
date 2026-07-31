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
using swordfs::volume::VolumeImpl;

namespace swordfs::vfs {

volume::VolumeImpl *VfsImpl::Volume() {
  return &volume::VolumeImpl::Instance();
}

void VfsImpl::SetRequestContext(fuse_req_t req) {
  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx = SwordFsContext{fuse_req_ctx(req)};
}

utils::Status VfsImpl::Lookup(fuse_ino_t parent, const char *name,
                              fuse_entry_param *entry) {
  InodeID child_ino;
  struct stat attr;
  Status status = VolumeImpl::Instance().meta_engine()->Lookup(parent, name, &child_ino,
                                              &attr);
  if (!status.ok()) return status;
  *entry = {};
  entry->ino = child_ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

void VfsImpl::Forget(fuse_ino_t ino, uint64_t nlookup) {
  VolumeImpl::Instance().meta_engine()->Forget(ino, nlookup);
}

utils::Status VfsImpl::Getattr(fuse_ino_t ino, struct stat *attr) {
  return VolumeImpl::Instance().meta_engine()->GetAttr(ino, attr);
}

utils::Status VfsImpl::Setattr(fuse_ino_t ino, struct stat *attr,
                               int to_set, struct stat *out_attr) {
  return VolumeImpl::Instance().meta_engine()->SetAttr(ino, attr, to_set, out_attr);
}

utils::Status VfsImpl::Readlink(fuse_ino_t ino) {
  (void)ino;
  return Status::NotSupported("readlink");
}

utils::Status VfsImpl::Mknod(fuse_ino_t parent, const char *name,
                             mode_t mode, dev_t rdev) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)rdev;
  return Status::NotSupported("mknod");
}

utils::Status VfsImpl::Mkdir(fuse_ino_t parent, const char *name,
                             mode_t mode, fuse_entry_param *entry) {
  InodeID child_ino;
  struct stat attr;
  Status status = VolumeImpl::Instance().meta_engine()->MkDir(parent, name, mode,
                                             &child_ino, &attr);
  if (!status.ok()) return status;
  *entry = {};
  entry->ino = child_ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

utils::Status VfsImpl::Unlink(fuse_ino_t parent, const char *name) {
  return VolumeImpl::Instance().meta_engine()->Unlink(parent, name);
}

utils::Status VfsImpl::Rmdir(fuse_ino_t parent, const char *name) {
  return VolumeImpl::Instance().meta_engine()->RmDir(parent, name);
}

utils::Status VfsImpl::Symlink(const char *link, fuse_ino_t parent,
                               const char *name) {
  (void)link;
  (void)parent;
  (void)name;
  return Status::NotSupported("symlink");
}

utils::Status VfsImpl::Rename(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags) {
  return VolumeImpl::Instance().meta_engine()->Rename(parent, name, newparent, newname, flags);
}

utils::Status VfsImpl::Link(fuse_ino_t ino, fuse_ino_t newparent,
                            const char *newname) {
  (void)ino;
  (void)newparent;
  (void)newname;
  return Status::NotSupported("link");
}

utils::Status VfsImpl::Open(fuse_ino_t ino, struct fuse_file_info *fi) {
  // Permission check and atime update (fh allocation moved to FileHandleManager).
  {
    uint64_t dummy;
    auto status = VolumeImpl::Instance().meta_engine()->Open(ino, &dummy);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " — " << status.message();
      return status;
    }
  }
  uint64_t fh;
  auto status = FileHandleManager::Instance().Open(ino, &fh);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " — " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Open: ino=" << ino << " fh=" << fh;
  fi->fh = fh;
  return Status::OK();
}

utils::Status VfsImpl::Read(fuse_ino_t ino, size_t size, off_t off,
                            uint64_t fh, std::unique_ptr<folly::IOBuf> *data) {
  SWORDFS_LOG_DEBUG << "Read: ino=" << ino << " offset=" << off << " size=" << size;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  auto buf = folly::IOBuf::create(size);
  auto status = handle->file_readwriter->Read(size, off, buf.get());
  if (!status.ok()) return status;
  *data = std::move(buf);
  return Status::OK();
}

utils::Status VfsImpl::Write(fuse_ino_t ino, const folly::IOBuf &buf,
                             off_t off, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Write: ino=" << ino << " fh=" << fh
                    << " size=" << buf.length() << " off=" << off;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  return handle->file_readwriter->Write(buf, off);
}

utils::Status VfsImpl::Flush(fuse_ino_t ino, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Flush: ino=" << ino << " fh=" << fh;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) return Status::OK();
  return handle->file_readwriter->Flush();
}

utils::Status VfsImpl::Release(fuse_ino_t ino, uint64_t fh) {
  FileHandleManager::Instance().Release(fh);
  Status status = VolumeImpl::Instance().meta_engine()->Release(fh);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Release FAILED: ino=" << ino << " fh=" << fh
                      << " — " << status.message();
  } else {
    SWORDFS_LOG_DEBUG << "Release: ino=" << ino << " fh=" << fh;
  }
  return status;
}

utils::Status VfsImpl::Fsync(fuse_ino_t ino, int datasync, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Fsync: ino=" << ino << " datasync=" << datasync
                    << " fh=" << fh;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) return Status::OK();
  return handle->file_readwriter->Flush();
}

utils::Status VfsImpl::Opendir(fuse_ino_t ino, uint64_t *fh) {
  // Permission check and atime update.
  {
    uint64_t dummy;
    auto status = VolumeImpl::Instance().meta_engine()->OpenDir(ino, &dummy);
    if (!status.ok()) return status;
  }
  *fh = FileHandleManager::Instance().OpenDir(ino);
  return Status::OK();
}

// Common implementation for Readdir and Readdirplus.
template <typename F>
static utils::Status ReaddirCommon(fuse_req_t req, fuse_ino_t ino, size_t size,
                                   off_t off,
                                   swordfs::metadata::IMetaEngine *meta,
                                   F &&add_entry, std::string *out) {
  using swordfs::metadata::SwordFsEntry;

  std::vector<SwordFsEntry> entries;
  Status status = meta->ReadDir(ino, &entries);
  if (!status.ok()) return status;

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
  if (!buf) return Status::NoMemory("readdir buffer");

  size_t pos = 0;
  for (size_t i = 0; i < entries.size() && pos < cap; ++i) {
    size_t n = add_entry(req, buf + pos, cap - pos,
                         entries[i], pos + sizes[i]);
    if (n > cap - pos) break;
    pos += n;
  }

  if (static_cast<size_t>(off) < pos)
    out->assign(buf + off, std::min(pos - off, size));
  std::free(buf);
  return Status::OK();
}

utils::Status VfsImpl::Readdir(fuse_ino_t ino, size_t size, off_t off,
                               std::string *buf) {
  // Need fuse_req_t for fuse_add_direntry.  The caller supplies context.
  return Status::OK();  // stub — caller handles via Vfs.cpp directly
}

utils::Status VfsImpl::Releasedir(fuse_ino_t ino, uint64_t fh) {
  (void)ino;
  FileHandleManager::Instance().ReleaseDir(fh);
  return VolumeImpl::Instance().meta_engine()->ReleaseDir(fh);
}

utils::Status VfsImpl::Fsyncdir(fuse_ino_t ino, int datasync) {
  (void)ino;
  (void)datasync;
  return Status::NotSupported("fsyncdir");
}

utils::Status VfsImpl::Statfs(fuse_ino_t ino, struct statvfs *stbuf) {
  (void)ino;
  return VolumeImpl::Instance().meta_engine()->StatFs(stbuf);
}

utils::Status VfsImpl::Setxattr(fuse_ino_t ino, const char *name,
                                const char *value, size_t size, int flags) {
  (void)ino;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  return Status::NotSupported("setxattr");
}

utils::Status VfsImpl::Getxattr(fuse_ino_t ino, const char *name,
                                size_t size) {
  (void)ino;
  (void)name;
  (void)size;
  return Status::NotSupported("getxattr");
}

utils::Status VfsImpl::Listxattr(fuse_ino_t ino, size_t size) {
  (void)ino;
  (void)size;
  return Status::NotSupported("listxattr");
}

utils::Status VfsImpl::Removexattr(fuse_ino_t ino, const char *name) {
  (void)ino;
  (void)name;
  return Status::NotSupported("removexattr");
}

utils::Status VfsImpl::Access(fuse_ino_t ino, int mask) {
  return VolumeImpl::Instance().meta_engine()->Access(ino, mask);
}

utils::Status VfsImpl::Create(fuse_ino_t parent, const char *name,
                              mode_t mode, fuse_entry_param *entry,
                              struct fuse_file_info *fi) {
  InodeID child_ino;
  struct stat attr;
  Status status = VolumeImpl::Instance().meta_engine()->Create(parent, name, mode, &child_ino, &attr);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create FAILED: parent=" << parent << " name='" << name
                      << "' — " << status.message();
    return status;
  }
  uint64_t fh;
  {
    uint64_t dummy;
    auto status = VolumeImpl::Instance().meta_engine()->Open(child_ino, &dummy);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Create: Open FAILED: ino=" << child_ino
                        << " — " << status.message();
      return status;
    }
  }
  auto status2 = FileHandleManager::Instance().Open(child_ino, &fh);
  if (!status2.ok()) {
    SWORDFS_LOG_ERROR << "Create: FileHandleManager::Open FAILED: ino=" << child_ino
                      << " — " << status2.message();
    return status2;
  }
  fi->fh = fh;
  *entry = {};
  entry->ino = child_ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  SWORDFS_LOG_INFO << "Create: ino=" << child_ino << " fh=" << fh
                   << " name='" << name << "'";
  return Status::OK();
}

utils::Status VfsImpl::Ioctl(fuse_ino_t ino, int cmd, void *arg,
                             struct fuse_file_info *fi, unsigned flags,
                             const void *in_buf, size_t in_bufsz,
                             size_t out_bufsz) {
  (void)ino;
  (void)cmd;
  (void)arg;
  (void)fi;
  (void)flags;
  (void)in_buf;
  (void)in_bufsz;
  (void)out_bufsz;
  return Status::NotSupported("ioctl");
}

utils::Status VfsImpl::RetrieveReply(fuse_req_t /*req*/, void *cookie,
                                     fuse_ino_t ino, off_t offset,
                                     struct fuse_bufvec *bufv) {
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  return Status::NotSupported("retrieve_reply");
}

void VfsImpl::ForgetMulti(fuse_req_t /*req*/, size_t count,
                          struct fuse_forget_data *forgets) {
  (void)count;
  (void)forgets;
}

utils::Status VfsImpl::Flock(fuse_ino_t ino,
                             struct fuse_file_info *fi, int op) {
  (void)ino;
  (void)fi;
  (void)op;
  return Status::NotSupported("flock");
}

utils::Status VfsImpl::Fallocate(fuse_ino_t ino, int mode,
                                 off_t offset, off_t length,
                                 struct fuse_file_info *fi) {
  (void)ino;
  (void)mode;
  (void)offset;
  (void)length;
  (void)fi;
  return Status::NotSupported("fallocate");
}

utils::Status VfsImpl::Readdirplus(fuse_ino_t ino, size_t size, off_t off,
                                   std::string *buf) {
  // Readdirplus requires fuse_add_direntry_plus which is a FUSE API.
  // Handled directly in Vfs.cpp; this stub returns empty.
  (void)ino;
  (void)size;
  (void)off;
  (void)buf;
  return Status::OK();
}

utils::Status VfsImpl::Lseek(fuse_ino_t ino, off_t off, int whence,
                             struct fuse_file_info *fi) {
  (void)ino;
  (void)off;
  (void)whence;
  (void)fi;
  return Status::NotSupported("lseek");
}

utils::Status VfsImpl::Tmpfile(fuse_ino_t parent, mode_t mode,
                               struct fuse_file_info *fi) {
  (void)parent;
  (void)mode;
  (void)fi;
  return Status::NotSupported("tmpfile");
}

utils::Status VfsImpl::Statx(fuse_ino_t ino, int flags, int mask,
                             struct fuse_file_info *fi) {
  (void)ino;
  (void)flags;
  (void)mask;
  (void)fi;
  return Status::NotSupported("statx");
}

}  // namespace swordfs::vfs
