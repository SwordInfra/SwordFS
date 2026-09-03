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
#include "vfs/DirHandle.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/FileReadWriter.hpp"
#include "vfs/Handle.hpp"
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

utils::Status VfsImpl::GetAttr(fuse_ino_t ino, struct stat *attr) {
  SwordFsInode inode;
  Status status = VolumeImpl::Instance().meta_engine()->GetInode(ino, &inode);
  if (status.ok()) {
    inode.attr.ToPosixStat(attr);
  }
  return status;
}

utils::Status VfsImpl::SetAttr(fuse_ino_t ino, struct stat *attr, int to_set, struct stat *out_attr) {
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

utils::Status VfsImpl::ReadLink(fuse_ino_t ino, std::string *target) {
  return VolumeImpl::Instance().meta_engine()->Readlink(ino, target);
}

utils::Status VfsImpl::MkNod(fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)rdev;
  return Status::NotSupported("mknod");
}

utils::Status VfsImpl::MkDir(fuse_ino_t parent, const char *name, mode_t mode, fuse_entry_param *entry) {
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
  status = meta->Unlink(parent, name, &post_nlink);
  if (!status.ok()) {
    return status;
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

utils::Status VfsImpl::RmDir(fuse_ino_t parent, const char *name) {
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
      auto status = handle->ReclaimData();
      if (!status.ok()) {
        SWORDFS_LOG_ERROR << "Rename: cleanup of overwritten inode " << result.overwritten_ino
                          << " failed: " << status.message();
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
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(ino, fi->flags, &handle);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " — " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Open: ino=" << ino << " fh=" << handle->fh();
  fi->fh = handle->fh();
  return Status::OK();
}

utils::Status VfsImpl::Read(fuse_ino_t ino, size_t size, off_t off, uint64_t fh, std::unique_ptr<folly::IOBuf> *data) {
  SWORDFS_LOG_DEBUG << "Read: ino=" << ino << " offset=" << off << " size=" << size;
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown file fh=" + std::to_string(fh));
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
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown file fh=" + std::to_string(fh));
  }
  auto status = handle->Write(buf, off);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "VfsImpl::Write FAILED: ino=" << ino << " fh=" << fh << " — " << status.message();
  }
  return status;
}

utils::Status VfsImpl::Flush(fuse_ino_t ino, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Flush: ino=" << ino << " fh=" << fh;
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown file fh=" + std::to_string(fh));
  }
  return handle->Flush();
}

utils::Status VfsImpl::Release(fuse_ino_t ino, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Release: ino=" << ino << " fh=" << fh;
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown file fh=" + std::to_string(fh));
  }
  return handle->Release();
}

utils::Status VfsImpl::FSync(fuse_ino_t ino, int datasync, uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Fsync: ino=" << ino << " datasync=" << datasync << " fh=" << fh;
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown file fh=" + std::to_string(fh));
  }
  return handle->Flush();
}

utils::Status VfsImpl::OpenDir(fuse_ino_t ino, uint64_t *fh) {
  std::shared_ptr<DirHandle> handle;
  auto status = DirHandle::Open(ino, &handle);
  if (status.ok()) {
    *fh = handle->fh();
  }
  return status;
}

// Common implementation for Readdir and Readdirplus. Directory iteration
// state belongs to the FUSE directory handle; the metadata iterator hides
// backend-specific directory-entry caching and continuation state.
class FuseDirEntryEncoder final : public DirEntryEncoder {
 public:
  enum class Mode {
    kNormal,
    kPlus,
  };

  FuseDirEntryEncoder(fuse_req_t req, Mode mode) : req_(req), mode_(mode) {
  }

  size_t CalSpace(const metadata::SwordFsEntry &entry, off_t next_off) const override {
    return AddEntry(entry, next_off, nullptr, 0);
  }

  void Encode(const metadata::SwordFsEntry &entry, off_t next_off, size_t required, std::string *out) const override {
    const size_t old_size = out->size();
    out->resize(old_size + required);
    AddEntry(entry, next_off, out->data() + old_size, required);
  }

 private:
  size_t AddEntry(const metadata::SwordFsEntry &entry, off_t next_off, char *buf, size_t capacity) const {
    if (mode_ == Mode::kPlus) {
      fuse_entry_param ep = {};
      ep.ino = entry.ino;
      ep.attr.st_ino = entry.ino;
      ep.attr.st_mode = entry.type << 12;
      ep.attr_timeout = 1.0;
      ep.entry_timeout = 1.0;
      return fuse_add_direntry_plus(req_, buf, capacity, entry.name.c_str(), &ep, next_off);
    }

    struct stat st = {};
    st.st_ino = entry.ino;
    st.st_mode = entry.type << 12;
    return fuse_add_direntry(req_, buf, capacity, entry.name.c_str(), &st, next_off);
  }

  fuse_req_t req_;
  Mode mode_;
};

static utils::Status ReaddirCommon(fuse_req_t req, size_t size, off_t off, uint64_t fh, FuseDirEntryEncoder::Mode mode,
                                   std::string *out) {
  auto handle = HandleManager::Instance().FindAs<DirHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown directory fh=" + std::to_string(fh));
  }

  FuseDirEntryEncoder encoder(req, mode);
  return handle->ReadDir(off, size, encoder, out);
}

utils::Status VfsImpl::ReadDir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, uint64_t fh, std::string *buf) {
  (void)ino;
  return ReaddirCommon(req, size, off, fh, FuseDirEntryEncoder::Mode::kNormal, buf);
}

utils::Status VfsImpl::ReadDirPlus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, uint64_t fh,
                                   std::string *buf) {
  (void)ino;
  return ReaddirCommon(req, size, off, fh, FuseDirEntryEncoder::Mode::kPlus, buf);
}

utils::Status VfsImpl::ReleaseDir(fuse_ino_t ino, uint64_t fh) {
  (void)ino;
  auto handle = HandleManager::Instance().FindAs<DirHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown directory fh=" + std::to_string(fh));
  }
  return handle->Release();
}

utils::Status VfsImpl::FSyncDir(fuse_ino_t ino, int datasync) {
  (void)ino;
  (void)datasync;
  return Status::NotSupported("fsyncdir");
}

utils::Status VfsImpl::StatFs(fuse_ino_t ino, struct statvfs *stbuf) {
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

utils::Status VfsImpl::SetXAttr(fuse_ino_t ino, const char *name, const char *value, size_t size, int flags) {
  (void)ino;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  return Status::NotSupported("setxattr");
}

utils::Status VfsImpl::GetXAttr(fuse_ino_t ino, const char *name, size_t size) {
  (void)ino;
  (void)name;
  (void)size;
  return Status::NotSupported("getxattr");
}

utils::Status VfsImpl::ListXAttr(fuse_ino_t ino, size_t size) {
  (void)ino;
  (void)size;
  return Status::NotSupported("listxattr");
}

utils::Status VfsImpl::RemoveXAttr(fuse_ino_t ino, const char *name) {
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
  std::shared_ptr<FileHandle> handle;
  status = FileHandle::Open(child.ino, fi->flags, &handle);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create: Open FAILED: ino=" << child.ino << " — " << status.message();
    return status;
  }
  fi->fh = handle->fh();
  *entry = {};
  entry->ino = child.ino;
  child.attr.ToPosixStat(&entry->attr);
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  SWORDFS_LOG_DEBUG << "Create: ino=" << child.ino << " fh=" << handle->fh() << " name='" << name << "'";
  return Status::OK();
}

utils::Status VfsImpl::IoCtl(fuse_ino_t ino, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
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

utils::Status VfsImpl::FLock(fuse_ino_t ino, struct fuse_file_info *fi, int op) {
  (void)ino;
  (void)fi;
  (void)op;
  return Status::NotSupported("flock");
}

utils::Status VfsImpl::FAllocate(fuse_ino_t ino, int mode, off_t offset, off_t length, struct fuse_file_info *fi) {
  (void)ino;
  (void)mode;
  (void)offset;
  (void)length;
  (void)fi;
  return Status::NotSupported("fallocate");
}

utils::Status VfsImpl::LSeek(fuse_ino_t ino, off_t off, int whence, struct fuse_file_info *fi) {
  (void)ino;
  (void)off;
  (void)whence;
  (void)fi;
  return Status::NotSupported("lseek");
}

utils::Status VfsImpl::TmpFile(fuse_ino_t parent, mode_t mode, struct fuse_file_info *fi) {
  (void)parent;
  (void)mode;
  (void)fi;
  return Status::NotSupported("tmpfile");
}

utils::Status VfsImpl::StatX(fuse_ino_t ino, int flags, int mask, struct fuse_file_info *fi) {
  (void)ino;
  (void)flags;
  (void)mask;
  (void)fi;
  return Status::NotSupported("statx");
}

}  // namespace swordfs::vfs
