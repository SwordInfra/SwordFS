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
#include "vfs/Reclaimer.hpp"
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
using swordfs::metadata::SwordFsInode;
using swordfs::volume::VolumeImpl;

namespace swordfs::vfs {

namespace {

utils::Status RefreshTrackedInode(metadata::SwordFsInode *inode) {
  auto handle = InodeHandleManager::Instance().Get(inode->ino, false);
  if (!handle) {
    return utils::Status::OK();
  }
  metadata::SwordFsInode refreshed;
  refreshed.ino = inode->ino;
  auto status = handle->GetAttr(&refreshed);
  if (!status.ok()) {
    return status;
  }
  *inode = std::move(refreshed);
  return utils::Status::OK();
}

}  // namespace

utils::Status VfsImpl::Lookup(fuse_ino_t parent, const char *name, fuse_entry_param *entry) {
  SwordFsInode child;
  Status status = VolumeImpl::Instance().meta_engine()->Lookup(parent, name, &child);
  if (!status.ok()) {
    return status;
  }
  status = RefreshTrackedInode(&child);
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
  auto handle = InodeHandleManager::Instance().Get(ino, false);
  Status status = handle ? handle->GetAttr(&inode) : VolumeImpl::Instance().meta_engine()->GetInode(ino, &inode);
  if (status.ok()) {
    inode.attr.ToPosixStat(attr);
  }
  return status;
}

utils::Status VfsImpl::SetAttr(fuse_ino_t ino, struct stat *attr, int to_set, std::optional<uint64_t> fh,
                               struct stat *out_attr) {
  SetAttrField fields = FromFuseSetAttrFields(to_set);
  SwordFsAttr metadata_attr = SwordFsAttr::FromPosixStat(*attr);
  SwordFsInode inode;
  Status status;
  if (fh.has_value()) {
    auto file_handle = HandleManager::Instance().FindAs<FileHandle>(*fh);
    if (!file_handle) {
      return Status::InvalidArgument("unknown file fh=" + std::to_string(*fh));
    }
    status = file_handle->SetAttr(metadata_attr, fields, out_attr ? &inode : nullptr);
  } else {
    auto inode_handle = InodeHandleManager::Instance().Get(ino, false);
    if (inode_handle) {
      status = inode_handle->SetAttr(metadata_attr, fields, out_attr ? &inode : nullptr);
    } else {
      status = VolumeImpl::Instance().meta_engine()->SetAttr(ino, metadata_attr, fields, out_attr ? &inode : nullptr);
    }
  }
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
  // The metadata mutation owns last-link detection and publishes durable
  // orphan work atomically. Foreground unlink never performs object-store
  // deletion; it only nudges the background reclaimer after commit.
  auto status = VolumeImpl::Instance().meta_engine()->Unlink(parent, name);
  if (!status.ok()) {
    return status;
  }
  Reclaimer::Instance().Wake();
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
  auto status = VolumeImpl::Instance().meta_engine()->Rename(parent, name, newparent, newname, rename_flags);
  if (!status.ok()) {
    return status;
  }
  // A non-overwriting rename creates no orphan, but Wake() is deliberately
  // cheap/coalesced and keeps VFS independent of metadata link-count details.
  Reclaimer::Instance().Wake();
  return utils::Status::OK();
}

utils::Status VfsImpl::Link(fuse_ino_t ino, fuse_ino_t newparent, const char *newname, fuse_entry_param *entry) {
  SwordFsInode inode;
  Status status = VolumeImpl::Instance().meta_engine()->Link(ino, newparent, newname, &inode);
  if (!status.ok()) {
    return status;
  }
  status = RefreshTrackedInode(&inode);
  if (!status.ok()) {
    // Link already committed successfully. Attribute enrichment must not turn
    // that namespace mutation into an apparent failure that callers may retry.
    // Keep the inode returned by Link; a later getattr can refresh it.
    SWORDFS_LOG_WARN << "Link live-attribute refresh failed after commit: ino=" << ino << " — " << status.message();
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
  auto status = handle->Release();
  // A close may have released the last local reference of a durable orphan.
  // Wake is cheap/coalesced and deliberately does not ask InodeHandle whether
  // this inode is orphaned; durable metadata remains the sole work authority.
  Reclaimer::Instance().Wake();
  return status;
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

// Directory iteration state belongs to the FUSE directory handle; the
// metadata iterator hides backend-specific directory-entry caching and
// continuation state. Plain READDIR stays entry-only; READDIRPLUS uses a
// separate encoder because it requires authoritative inode metadata.
class FuseDirEntryEncoder final : public DirEntryEncoder {
 public:
  explicit FuseDirEntryEncoder(fuse_req_t req) : req_(req) {
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
    struct stat st = {};
    st.st_ino = entry.ino;
    st.st_mode = entry.type << 12;
    return fuse_add_direntry(req_, buf, capacity, entry.name.c_str(), &st, next_off);
  }

  fuse_req_t req_;
};

class FuseDirEntryPlusEncoder final : public DirEntryPlusEncoder {
 public:
  explicit FuseDirEntryPlusEncoder(fuse_req_t req) : req_(req) {
  }

  size_t CalSpace(const metadata::SwordFsEntry &entry, off_t next_off) const override {
    fuse_entry_param ep = {};
    return fuse_add_direntry_plus(req_, nullptr, 0, entry.name.c_str(), &ep, next_off);
  }

  void Encode(const metadata::SwordFsEntry &entry, const metadata::SwordFsInode &inode, off_t next_off, size_t required,
              std::string *out) const override {
    const size_t old_size = out->size();
    out->resize(old_size + required);
    AddEntry(entry, inode, next_off, out->data() + old_size, required);
  }

 private:
  size_t AddEntry(const metadata::SwordFsEntry &entry, const metadata::SwordFsInode &inode, off_t next_off, char *buf,
                  size_t capacity) const {
    fuse_entry_param ep = {};
    ep.ino = inode.ino;
    inode.attr.ToPosixStat(&ep.attr);
    // SwordFS does not yet have a cross-mount dentry/inode invalidation or
    // lease mechanism. Zero validity prevents READDIRPLUS from publishing a
    // stale cache promise while still supplying correct attributes.
    ep.attr_timeout = 0.0;
    ep.entry_timeout = 0.0;
    return fuse_add_direntry_plus(req_, buf, capacity, entry.name.c_str(), &ep, next_off);
  }

  fuse_req_t req_;
};

utils::Status VfsImpl::ReadDir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, uint64_t fh, std::string *buf) {
  (void)ino;
  auto handle = HandleManager::Instance().FindAs<DirHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown directory fh=" + std::to_string(fh));
  }
  FuseDirEntryEncoder encoder(req);
  return handle->ReadDir(off, size, encoder, buf);
}

utils::Status VfsImpl::ReadDirPlus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, uint64_t fh,
                                   std::string *buf) {
  (void)ino;
  auto handle = HandleManager::Instance().FindAs<DirHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown directory fh=" + std::to_string(fh));
  }
  FuseDirEntryPlusEncoder encoder(req);
  return handle->ReadDirPlus(off, size, encoder, buf);
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

utils::Status VfsImpl::Create(fuse_ino_t parent, const char *name, mode_t mode, fuse_entry_param *entry,
                              struct fuse_file_info *fi) {
  SwordFsInode child;
  Status status = VolumeImpl::Instance().meta_engine()->Create(parent, name, static_cast<uint32_t>(mode), &child);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create FAILED: parent=" << parent << " name='" << name << "' — " << status.message();
    return status;
  }
  std::shared_ptr<FileHandle> handle;
  status = FileHandle::Create(child.ino, fi->flags, &handle);
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
