// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/VfsImpl.hpp"

#include <dirent.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include "config/ConfigCenter.hpp"
#include "fuse/Limits.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/FileReadWriter.hpp"
#include "vfs/InodeHandle.hpp"
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

utils::Status VfsImpl::Lookup(fuse_ino_t parent, const char *name,
                              fuse_entry_param *entry) {
  InodeID child_ino;
  struct stat attr;
  Status status = VolumeImpl::Instance().meta_engine()->Lookup(parent, name, &child_ino,
                                                               &attr);
  if (!status.ok()) {
    return status;
  }
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

utils::Status VfsImpl::Readlink(fuse_ino_t ino, std::string *target) {
  return VolumeImpl::Instance().meta_engine()->Readlink(ino, target);
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
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = child_ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

utils::Status VfsImpl::Unlink(fuse_ino_t parent, const char *name) {
  // POSIX unlink decision lives here, not in the metadata engine. Three
  // states the inode can be in after `Unlink`:
  //   - nlink > 0: at least one directory entry still references the
  //     inode (hard-link). Do nothing — the chunk objects and inode
  //     stay alive for the other names.
  //   - nlink == 0 && no open fd: fully delete the chunks + the inode
  //     now via `ReclaimData`.
  //   - nlink == 0 && some fd still open: mark the InodeHandle as
  //     orphaned and let the last `Close` call `ReclaimData`.
  //
  // Permission and sticky-bit checks are already enforced by
  // `MemMetaImpl::Unlink` above the store, so we don't repeat them here.
  auto *meta = VolumeImpl::Instance().meta_engine();

  // Look up the child inode id.
  InodeID child_ino = 0;
  struct stat lookup_attr;
  auto status = meta->Lookup(parent, name, &child_ino, &lookup_attr);
  if (!status.ok()) {
    return status;
  }

  // meta->Unlink hands back the authoritative post-decrement nlink.
  nlink_t post_nlink = 0;
  auto st = meta->Unlink(parent, name, &post_nlink);
  if (!st.ok()) {
    return st;
  }

  // Hardlink still alive? Then the inode (and its chunks) belong to
  // another name; leave them alone.
  if (post_nlink > 0) {
    return utils::Status::OK();
  }

  auto handle = vfs::InodeHandleManager::Instance().Get(child_ino, /*create_if_missing=*/true);
  if (!handle) {
    return utils::Status::Internal("failed to get InodeHandle");
  }
  if (!handle->MarkOrphanedIfOpen()) {
    // ReclaimData removes both the chunk objects (via the data engine)
    // and the inode (via the metadata engine).
    return handle->ReclaimData();
  }
  // Defer the actual cleanup until the last `Close`.
  return utils::Status::OK();
}

utils::Status VfsImpl::Rmdir(fuse_ino_t parent, const char *name) {
  return VolumeImpl::Instance().meta_engine()->RmDir(parent, name);
}

utils::Status VfsImpl::Symlink(const char *link, fuse_ino_t parent,
                               const char *name,
                               fuse_entry_param *entry) {
  InodeID child_ino;
  struct stat attr;
  Status status = VolumeImpl::Instance().meta_engine()->Symlink(
      parent, name, link, &child_ino, &attr);
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = child_ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

utils::Status VfsImpl::Rename(fuse_ino_t parent, const char *name,
                              fuse_ino_t newparent, const char *newname,
                              unsigned int flags) {
  return VolumeImpl::Instance().meta_engine()->Rename(parent, name, newparent, newname, flags);
}

utils::Status VfsImpl::Link(fuse_ino_t ino, fuse_ino_t newparent,
                            const char *newname,
                            fuse_entry_param *entry) {
  struct stat attr;
  Status status = VolumeImpl::Instance().meta_engine()->Link(
      ino, newparent, newname, &attr);
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

utils::Status VfsImpl::Open(fuse_ino_t ino, struct fuse_file_info *fi) {
  FileHandle handle;
  auto status = FileHandle::Open(ino, fi->flags, &handle);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " — " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Open: ino=" << ino << " fh=" << handle.fh();
  fi->fh = handle.fh();
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
  auto status = handle->Read(size, off, buf.get());
  if (!status.ok()) {
    return status;
  }
  *data = std::move(buf);
  return Status::OK();
}

utils::Status VfsImpl::Write(fuse_ino_t ino, const folly::IOBuf &buf,
                             off_t off, uint64_t fh) {
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  auto status = handle->Write(buf, off);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "VfsImpl::Write FAILED: ino=" << ino
                      << " fh=" << fh << " — " << status.message();
  }
  return status;
}

utils::Status VfsImpl::Flush(fuse_ino_t ino, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Flush: ino=" << ino << " fh=" << fh;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  return handle->Flush();
}

utils::Status VfsImpl::Release(fuse_ino_t ino, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Release: ino=" << ino << " fh=" << fh;
  FileHandleManager::Instance().Release(fh);
  return Status::OK();
}

utils::Status VfsImpl::Fsync(fuse_ino_t ino, int datasync, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Fsync: ino=" << ino << " datasync=" << datasync
                    << " fh=" << fh;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  return handle->Flush();
}

utils::Status VfsImpl::Opendir(fuse_ino_t ino, uint64_t *fh) {
  // Permission check and atime update.
  auto status = VolumeImpl::Instance().meta_engine()->OpenDir(ino);
  if (!status.ok()) {
    return status;
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
  if (!status.ok()) {
    return status;
  }

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
    return Status::NoMemory("readdir buffer");
  }

  size_t pos = 0;
  for (size_t i = 0; i < entries.size() && pos < cap; ++i) {
    size_t n = add_entry(req, buf + pos, cap - pos,
                         entries[i], pos + sizes[i]);
    if (n > cap - pos) {
      break;
    }
    pos += n;
  }

  if (static_cast<size_t>(off) < pos) {
    out->assign(buf + off, std::min(pos - off, size));
  }
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
  return Status::OK();
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
  FileHandle handle;
  status = FileHandle::Open(child_ino, fi->flags, &handle);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create: Open FAILED: ino=" << child_ino
                      << " — " << status.message();
    return status;
  }
  fi->fh = handle.fh();
  *entry = {};
  entry->ino = child_ino;
  entry->attr = attr;
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  SWORDFS_LOG_DEBUG << "Create: ino=" << child_ino << " fh=" << handle.fh()
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
