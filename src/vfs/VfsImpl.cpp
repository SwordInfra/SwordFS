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

using swordfs::metadata::FromFuseRenameFlags;
using swordfs::metadata::FromFuseSetAttrFields;
using swordfs::metadata::InodeID;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::volume::VolumeImpl;

namespace swordfs::vfs {

volume::VolumeImpl *VfsImpl::Volume() {
  return &volume::VolumeImpl::Instance();
}

utils::Status VfsImpl::Lookup(fuse_ino_t parent, const char *name, fuse_entry_param *entry) {
  SwordFsInode child;
  Status status = VolumeImpl::Instance().meta_engine()->Lookup(parent, name, &child);
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = child.ino;
  child.attr.ToPosixStat(&entry->attr);
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

utils::Status VfsImpl::Getattr(fuse_ino_t ino, struct stat *attr) {
  SwordFsInode inode;
  Status status = VolumeImpl::Instance().meta_engine()->GetInode(ino, &inode);
  if (status.ok()) {
    inode.attr.ToPosixStat(attr);
  }
  return status;
}

utils::Status VfsImpl::Setattr(fuse_ino_t ino, struct stat *attr, int to_set, struct stat *out_attr) {
  SetAttrField fields = FromFuseSetAttrFields(to_set);
  SwordFsAttr metadata_attr = SwordFsAttr::FromPosixStat(*attr);
  SwordFsInode inode;
  Status status =
      VolumeImpl::Instance().meta_engine()->SetAttr(ino, metadata_attr, fields, out_attr ? &inode : nullptr);
  if (status.ok() && out_attr) {
    inode.attr.ToPosixStat(out_attr);
  }
  return status;
}

utils::Status VfsImpl::Readlink(fuse_ino_t ino, std::string *target) {
  return VolumeImpl::Instance().meta_engine()->Readlink(ino, target);
}

utils::Status VfsImpl::Mknod(fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)rdev;
  return Status::NotSupported("mknod");
}

utils::Status VfsImpl::Mkdir(fuse_ino_t parent, const char *name, mode_t mode, fuse_entry_param *entry) {
  SwordFsInode child;
  Status status = VolumeImpl::Instance().meta_engine()->MkDir(parent, name, static_cast<uint32_t>(mode), &child);
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = child.ino;
  child.attr.ToPosixStat(&entry->attr);
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

  // Look up the child inode.
  SwordFsInode child;
  auto status = meta->Lookup(parent, name, &child);
  if (!status.ok()) {
    return status;
  }

  // meta->Unlink hands back the authoritative post-decrement nlink.
  uint64_t post_nlink = 0;
  auto st = meta->Unlink(parent, name, &post_nlink);
  if (!st.ok()) {
    return st;
  }

  // Hardlink still alive? Then the inode (and its chunks) belong to
  // another name; leave them alone.
  if (post_nlink > 0) {
    return utils::Status::OK();
  }

  auto handle = vfs::InodeHandleManager::Instance().Get(child.ino, /*create_if_missing=*/true);
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

utils::Status VfsImpl::Symlink(const char *link, fuse_ino_t parent, const char *name, fuse_entry_param *entry) {
  SwordFsInode child;
  Status status = VolumeImpl::Instance().meta_engine()->Symlink(parent, name, std::string_view(link), &child);
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = child.ino;
  child.attr.ToPosixStat(&entry->attr);
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  return Status::OK();
}

utils::Status VfsImpl::Rename(fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname,
                              unsigned int flags) {
  RenameFlag rename_flags = FromFuseRenameFlags(flags);
  auto meta = VolumeImpl::Instance().meta_engine();
  metadata::RenameResult result;
  auto status = meta->Rename(parent, name, newparent, newname, rename_flags, &result);
  if (!status.ok()) {
    return status;
  }

  // A rename-overwrite can orphan the replaced file just like unlink(2).
  // The metadata transaction deliberately does not reclaim it because it
  // cannot know whether an open file descriptor still references it. Let
  // InodeHandle perform the same data/inode cleanup used by unlink.
  if (result.overwritten_ino != 0 && result.overwritten_post_nlink == 0) {
    auto handle = vfs::InodeHandleManager::Instance().Get(result.overwritten_ino, /*create_if_missing=*/true);
    if (!handle) {
      // The metadata rename has already committed, so returning an error
      // here would report a failed rename for a state that is already
      // visible. Cleanup is best-effort after the metadata transaction;
      // log the failure. A future orphan-data reconciliation mechanism
      // should provide retry/repair for cleanup failures.
      SWORDFS_LOG_ERROR << "Rename: failed to get InodeHandle for overwritten " << "inode " << result.overwritten_ino
                        << "; rename has already committed";
    } else if (!handle->MarkOrphanedIfOpen()) {
      auto cleanup_status = handle->ReclaimData();
      if (!cleanup_status.ok()) {
        SWORDFS_LOG_ERROR << "Rename: cleanup of overwritten inode " << result.overwritten_ino
                          << " failed: " << cleanup_status.message();
      }
    }
  }
  return utils::Status::OK();
}

utils::Status VfsImpl::Link(fuse_ino_t ino, fuse_ino_t newparent, const char *newname, fuse_entry_param *entry) {
  SwordFsInode inode;
  Status status = VolumeImpl::Instance().meta_engine()->Link(ino, newparent, newname, &inode);
  if (!status.ok()) {
    return status;
  }
  *entry = {};
  entry->ino = inode.ino;
  inode.attr.ToPosixStat(&entry->attr);
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

utils::Status VfsImpl::Read(fuse_ino_t ino, size_t size, off_t off, uint64_t fh, std::unique_ptr<folly::IOBuf> *data) {
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

utils::Status VfsImpl::Write(fuse_ino_t ino, const folly::IOBuf &buf, off_t off, uint64_t fh) {
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  auto status = handle->Write(buf, off);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "VfsImpl::Write FAILED: ino=" << ino << " fh=" << fh << " — " << status.message();
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
  return FileHandleManager::Instance().Release(fh);
}

utils::Status VfsImpl::Fsync(fuse_ino_t ino, int datasync, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Fsync: ino=" << ino << " datasync=" << datasync << " fh=" << fh;
  auto handle = FileHandleManager::Instance().Find(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown fh=" + std::to_string(fh));
  }
  return handle->Flush();
}

utils::Status VfsImpl::Opendir(fuse_ino_t ino, uint64_t *fh) {
  if (fh == nullptr) {
    return Status::InvalidArgument("directory handle output is null");
  }
  auto *meta = VolumeImpl::Instance().meta_engine();
  auto status = meta->OpenDir(ino);
  if (!status.ok()) {
    return status;
  }
  std::unique_ptr<metadata::IDirIterator> iterator;
  status = meta->OpenDirIterator(ino, &iterator);
  if (!status.ok()) {
    return status;
  }
  auto shared_iterator = std::shared_ptr<metadata::IDirIterator>(std::move(iterator));
  *fh = FileHandleManager::Instance().OpenDir(std::move(shared_iterator));
  if (*fh == 0) {
    return Status::NoMemory("directory handle");
  }
  return Status::OK();
}

// Common implementation for Readdir and Readdirplus. Directory iteration
// state belongs to the FUSE directory handle; the metadata iterator hides
// backend-specific continuation state such as a Redis HSCAN cursor.
template <typename F>
static utils::Status ReaddirCommon(fuse_req_t req, size_t size, off_t off, uint64_t fh, F &&add_entry,
                                   std::string *out) {
  auto dir_handle = FileHandleManager::Instance().FindDir(fh);
  if (!dir_handle) {
    return Status::InvalidArgument("unknown directory fh=" + std::to_string(fh));
  }
  if (off < 0) {
    return Status::InvalidArgument("negative directory offset");
  }

  // Serialize readdir calls for a directory handle so the Peek + Read pair
  // below is atomic with respect to other callers using the same fh.
  std::lock_guard lock(dir_handle->mutex);
  auto &iterator = dir_handle->iterator;

  // A zero-sized request is valid and must not advance the iterator.
  if (size == 0) {
    out->clear();
    return Status::OK();
  }

  // Peek before consuming each entry so a byte-sized FUSE buffer boundary
  // never drops an entry. The iterator advances only after the entry has
  // been encoded successfully.
  out->clear();
  uint64_t current_off = static_cast<uint64_t>(off);
  while (out->size() < size) {
    metadata::SwordFsEntry entry;
    uint64_t next_off = current_off;
    bool end = false;
    Status status = iterator->Peek(current_off, &entry, &next_off, &end);
    if (status.IsNotFound()) {
      break;
    }
    if (!status.ok()) {
      return status;
    }

    const size_t required = add_entry(req, nullptr, 0, entry, static_cast<off_t>(next_off));
    const size_t remaining = size - out->size();
    if (required > remaining) {
      if (out->empty()) {
        return Status::NoMemory("readdir entry does not fit in buffer");
      }
      break;
    }

    std::string encoded(required, '\0');
    const size_t written = add_entry(req, encoded.data(), encoded.size(), entry, static_cast<off_t>(next_off));
    if (written > encoded.size()) {
      return Status::NoMemory("readdir entry encoding overflow");
    }
    std::vector<metadata::SwordFsEntry> consumed;
    uint64_t consumed_off = current_off;
    bool consumed_end = false;
    status = iterator->Read(current_off, 1, &consumed, &consumed_off, &consumed_end);
    if (!status.ok() || consumed.size() != 1 || consumed.front().ino != entry.ino ||
        consumed.front().name != entry.name) {
      return status.ok() ? Status::Internal("directory iterator changed between peek and read") : status;
    }
    out->append(encoded.data(), written);
    current_off = consumed_off;
    if (consumed_end) {
      break;
    }
  }
  return Status::OK();
}

utils::Status VfsImpl::Readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, uint64_t fh, std::string *buf) {
  (void)ino;
  return ReaddirCommon(
      req, size, off, fh,
      [](fuse_req_t r, char *p, size_t cap, const metadata::SwordFsEntry &e, off_t next_off) {
        struct stat st = {};
        st.st_ino = e.ino;
        st.st_mode = e.type << 12;
        return fuse_add_direntry(r, p, cap, e.name.c_str(), &st, next_off);
      },
      buf);
}

utils::Status VfsImpl::Readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, uint64_t fh,
                                   std::string *buf) {
  (void)ino;
  return ReaddirCommon(
      req, size, off, fh,
      [](fuse_req_t r, char *p, size_t cap, const metadata::SwordFsEntry &e, off_t next_off) {
        fuse_entry_param ep = {};
        ep.ino = e.ino;
        ep.attr.st_ino = e.ino;
        ep.attr.st_mode = e.type << 12;
        ep.attr_timeout = 1.0;
        ep.entry_timeout = 1.0;
        return fuse_add_direntry_plus(r, p, cap, e.name.c_str(), &ep, next_off);
      },
      buf);
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
  if (stbuf == nullptr) {
    return Status::InvalidArgument("statfs output is null");
  }
  metadata::SwordFsStatFs stats;
  auto status = VolumeImpl::Instance().meta_engine()->StatFs(&stats);
  if (!status.ok()) {
    return status;
  }
  std::memset(stbuf, 0, sizeof(*stbuf));
  stbuf->f_namemax = stats.name_max;
  stbuf->f_frsize = stats.fragment_size;
  stbuf->f_bsize = stats.block_size;
  stbuf->f_blocks = stats.blocks;
  stbuf->f_bfree = stats.blocks_free;
  stbuf->f_bavail = stats.blocks_available;
  stbuf->f_files = stats.files;
  stbuf->f_ffree = stats.files_free;
  stbuf->f_favail = stats.files_free;
  return Status::OK();
}

utils::Status VfsImpl::Setxattr(fuse_ino_t ino, const char *name, const char *value, size_t size, int flags) {
  (void)ino;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  return Status::NotSupported("setxattr");
}

utils::Status VfsImpl::Getxattr(fuse_ino_t ino, const char *name, size_t size) {
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
  return VolumeImpl::Instance().meta_engine()->Access(ino, static_cast<uint32_t>(mask));
}

utils::Status VfsImpl::Create(fuse_ino_t parent, const char *name, mode_t mode, fuse_entry_param *entry,
                              struct fuse_file_info *fi) {
  SwordFsInode child;
  Status status = VolumeImpl::Instance().meta_engine()->Create(parent, name, static_cast<uint32_t>(mode), &child);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create FAILED: parent=" << parent << " name='" << name << "' — " << status.message();
    return status;
  }
  FileHandle handle;
  status = FileHandle::Open(child.ino, fi->flags, &handle);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create: Open FAILED: ino=" << child.ino << " — " << status.message();
    return status;
  }
  fi->fh = handle.fh();
  *entry = {};
  entry->ino = child.ino;
  child.attr.ToPosixStat(&entry->attr);
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  SWORDFS_LOG_DEBUG << "Create: ino=" << child.ino << " fh=" << handle.fh() << " name='" << name << "'";
  return Status::OK();
}

utils::Status VfsImpl::Ioctl(fuse_ino_t ino, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
                             const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
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

utils::Status VfsImpl::RetrieveReply(fuse_req_t /*req*/, void *cookie, fuse_ino_t ino, off_t offset,
                                     struct fuse_bufvec *bufv) {
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  return Status::NotSupported("retrieve_reply");
}

utils::Status VfsImpl::Flock(fuse_ino_t ino, struct fuse_file_info *fi, int op) {
  (void)ino;
  (void)fi;
  (void)op;
  return Status::NotSupported("flock");
}

utils::Status VfsImpl::Fallocate(fuse_ino_t ino, int mode, off_t offset, off_t length, struct fuse_file_info *fi) {
  (void)ino;
  (void)mode;
  (void)offset;
  (void)length;
  (void)fi;
  return Status::NotSupported("fallocate");
}

utils::Status VfsImpl::Lseek(fuse_ino_t ino, off_t off, int whence, struct fuse_file_info *fi) {
  (void)ino;
  (void)off;
  (void)whence;
  (void)fi;
  return Status::NotSupported("lseek");
}

utils::Status VfsImpl::Tmpfile(fuse_ino_t parent, mode_t mode, struct fuse_file_info *fi) {
  (void)parent;
  (void)mode;
  (void)fi;
  return Status::NotSupported("tmpfile");
}

utils::Status VfsImpl::Statx(fuse_ino_t ino, int flags, int mask, struct fuse_file_info *fi) {
  (void)ino;
  (void)flags;
  (void)mask;
  (void)fi;
  return Status::NotSupported("statx");
}

}  // namespace swordfs::vfs
