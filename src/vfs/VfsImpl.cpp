// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/VfsImpl.hpp"

#include <dirent.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>
#include <linux/fs.h>
#include <sys/xattr.h>

#include "config/ConfigCenter.hpp"
#include "fuse/Limits.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Utils.hpp"
#include "runtime/MountRuntimeBehavior.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"
#include "vfs/DirHandle.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/FileReadWriter.hpp"
#include "vfs/FuseInodeCache.hpp"
#include "vfs/Handle.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/OrphanReclaimer.hpp"
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
using swordfs::metadata::InodeFlag;
using swordfs::metadata::InodeID;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::XAttrSetMode;
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

utils::Status ResolveLiveInode(fuse_ino_t ino, metadata::SwordFsInode *inode) {
  auto handle = InodeHandleManager::Instance().Get(ino, false);
  Status status = handle ? handle->GetAttr(inode) : VolumeImpl::Instance().meta_engine()->GetInode(ino, inode);
  if (status.IsNotFound() && FuseInodeCache::Instance().ResolveDetachedAfterNotFound(ino, inode)) {
    status = Status::OK();
  }
  if (status.ok()) {
    FuseInodeCache::Instance().RefreshIfRetained(*inode);
  }
  return status;
}

utils::Status ValidateUserXAttrName(const char *name) {
  if (name == nullptr || name[0] == '\0') {
    return utils::Status::InvalidArgument("xattr name is empty");
  }
  constexpr std::string_view kUserPrefix = "user.";
  const std::string_view xattr_name(name);
  if (xattr_name.size() > metadata::kMaxXAttrNameLength) {
    return utils::Status::Range("xattr name exceeds maximum length");
  }
  if (xattr_name == kUserPrefix) {
    return utils::Status::InvalidArgument("xattr name is empty");
  }
  if (!xattr_name.starts_with(kUserPrefix)) {
    return utils::Status::OperationNotSupported("xattr namespace is not supported");
  }
  return utils::Status::OK();
}

utils::Status TranslateXAttrSetMode(int flags, XAttrSetMode &mode) {
  switch (flags) {
    case 0:
      mode = XAttrSetMode::kUpsert;
      return utils::Status::OK();
    case XATTR_CREATE:
      mode = XAttrSetMode::kCreateOnly;
      return utils::Status::OK();
    case XATTR_REPLACE:
      mode = XAttrSetMode::kReplaceOnly;
      return utils::Status::OK();
    default:
      return utils::Status::InvalidArgument("invalid setxattr flags");
  }
}

InodeFlag FromLegacyInodeFlags(uint32_t flags) {
  InodeFlag result = InodeFlag::kNone;
  if ((flags & FS_IMMUTABLE_FL) != 0) {
    result = result | InodeFlag::kImmutable;
  }
  if ((flags & FS_APPEND_FL) != 0) {
    result = result | InodeFlag::kAppendOnly;
  }
  return result;
}

uint32_t ToLegacyInodeFlags(InodeFlag inode_flags) {
  uint32_t result = 0;
  if (metadata::HasInodeFlag(inode_flags, InodeFlag::kImmutable)) {
    result |= FS_IMMUTABLE_FL;
  }
  if (metadata::HasInodeFlag(inode_flags, InodeFlag::kAppendOnly)) {
    result |= FS_APPEND_FL;
  }
  return result;
}

InodeFlag FromFsXFlags(uint32_t flags) {
  InodeFlag result = InodeFlag::kNone;
  if ((flags & FS_XFLAG_IMMUTABLE) != 0) {
    result = result | InodeFlag::kImmutable;
  }
  if ((flags & FS_XFLAG_APPEND) != 0) {
    result = result | InodeFlag::kAppendOnly;
  }
  return result;
}

uint32_t ToFsXFlags(InodeFlag inode_flags) {
  uint32_t result = 0;
  if (metadata::HasInodeFlag(inode_flags, InodeFlag::kImmutable)) {
    result |= FS_XFLAG_IMMUTABLE;
  }
  if (metadata::HasInodeFlag(inode_flags, InodeFlag::kAppendOnly)) {
    result |= FS_XFLAG_APPEND;
  }
  return result;
}

utils::Status SetLocalAwareInodeFlags(InodeID ino, InodeFlag inode_flags) {
  auto handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/false);
  if (handle != nullptr) {
    return handle->SetInodeFlags(inode_flags, nullptr);
  }
  return VolumeImpl::Instance().meta_engine()->SetInodeFlags(ino, inode_flags, nullptr);
}

template <typename T>
void SetIoCtlOutput(const T &value, IoCtlReply *reply) {
  reply->output.assign(reinterpret_cast<const char *>(&value), sizeof(value));
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
  FuseInodeCache::Instance().RetainLookup(child);
  return Status::OK();
}

utils::Status VfsImpl::GetAttr(fuse_ino_t ino, struct stat *attr) {
  SwordFsInode inode;
  Status status = ResolveLiveInode(ino, &inode);
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
    FuseInodeCache::Instance().RefreshIfRetained(inode);
    inode.attr.ToPosixStat(out_attr);
  }
  return status;
}

utils::Status VfsImpl::ReadLink(fuse_ino_t ino, std::string *target) {
  return VolumeImpl::Instance().meta_engine()->Readlink(ino, target);
}

utils::Status VfsImpl::MkNod(fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev, fuse_entry_param *entry) {
  if (auto status = metadata::ValidateMknodMode(static_cast<uint32_t>(mode)); !status.ok()) {
    return status;
  }

  SwordFsInode child;
  auto status = VolumeImpl::Instance().meta_engine()->MkNod(parent, name, static_cast<uint32_t>(mode),
                                                            static_cast<uint64_t>(rdev), &child);
  if (!status.ok()) {
    return status;
  }

  *entry = {};
  entry->ino = child.ino;
  child.attr.ToPosixStat(&entry->attr);
  entry->attr_timeout = 1.0;
  entry->entry_timeout = 1.0;
  FuseInodeCache::Instance().RetainLookup(child);
  return Status::OK();
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
  FuseInodeCache::Instance().RetainLookup(child);
  return Status::OK();
}

utils::Status VfsImpl::Unlink(fuse_ino_t parent, const char *name) {
  // The metadata mutation owns last-link detection and publishes durable
  // orphan work atomically. Foreground unlink never performs object-store
  // deletion; it only nudges OrphanReclaimer after commit.
  auto status = VolumeImpl::Instance().meta_engine()->Unlink(parent, name);
  if (!status.ok()) {
    return status;
  }
  OrphanReclaimer::Instance().Wake();
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
  FuseInodeCache::Instance().RetainLookup(child);
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
  OrphanReclaimer::Instance().Wake();
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
  FuseInodeCache::Instance().RetainLookup(inode);
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
  OrphanReclaimer::Instance().Wake();
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
  FuseDirEntryPlusEncoder(fuse_req_t req, std::vector<fuse_ino_t> *retained_lookups)
      : req_(req), retained_lookups_(retained_lookups) {
  }

  size_t CalSpace(const metadata::SwordFsEntry &entry, off_t next_off) const override {
    fuse_entry_param ep = {};
    return fuse_add_direntry_plus(req_, nullptr, 0, entry.name.c_str(), &ep, next_off);
  }

  void Encode(const metadata::SwordFsEntry &entry, const metadata::SwordFsInode &inode, off_t next_off, size_t required,
              std::string *out) const override {
    if (entry.name != "." && entry.name != "..") {
      FuseInodeCache::Instance().RetainLookup(inode);
      if (retained_lookups_ != nullptr) {
        retained_lookups_->push_back(inode.ino);
      }
    }
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
  std::vector<fuse_ino_t> *retained_lookups_;
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
                                   std::string *buf, std::vector<fuse_ino_t> *retained_lookups) {
  (void)ino;
  if (retained_lookups != nullptr) {
    retained_lookups->clear();
  }
  auto handle = HandleManager::Instance().FindAs<DirHandle>(fh);
  if (!handle) {
    return Status::InvalidArgument("unknown directory fh=" + std::to_string(fh));
  }
  FuseDirEntryPlusEncoder encoder(req, retained_lookups);
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
  auto status = ValidateUserXAttrName(name);
  if (!status.ok()) {
    return status;
  }
  if (value == nullptr && size != 0) {
    return Status::InvalidArgument("xattr value is null");
  }
  if (size > metadata::kMaxXAttrValueSize) {
    return Status::Range("xattr value exceeds maximum size");
  }
  XAttrSetMode mode = XAttrSetMode::kUpsert;
  status = TranslateXAttrSetMode(flags, mode);
  if (!status.ok()) {
    return status;
  }
  const std::string_view xattr_value = size == 0 ? std::string_view{} : std::string_view(value, size);
  return VolumeImpl::Instance().meta_engine()->SetXAttr(ino, name, xattr_value, mode);
}

utils::Status VfsImpl::GetXAttr(fuse_ino_t ino, const char *name, std::string *value) {
  auto status = ValidateUserXAttrName(name);
  if (!status.ok()) {
    return status;
  }
  if (value == nullptr) {
    return Status::InvalidArgument("xattr value output is null");
  }
  return VolumeImpl::Instance().meta_engine()->GetXAttr(ino, name, value);
}

utils::Status VfsImpl::ListXAttrs(fuse_ino_t ino, std::vector<std::string> *names) {
  if (names == nullptr) {
    return Status::InvalidArgument("xattr names output is null");
  }
  auto status = VolumeImpl::Instance().meta_engine()->ListXAttrs(ino, names);
  if (!status.ok()) {
    return status;
  }
  std::erase_if(*names, [](const std::string &name) {
    constexpr std::string_view kUserPrefix = "user.";
    return !std::string_view(name).starts_with(kUserPrefix) || name.size() == kUserPrefix.size();
  });
  return Status::OK();
}

utils::Status VfsImpl::RemoveXAttr(fuse_ino_t ino, const char *name) {
  auto status = ValidateUserXAttrName(name);
  if (!status.ok()) {
    return status;
  }
  return VolumeImpl::Instance().meta_engine()->RemoveXAttr(ino, name);
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
  FuseInodeCache::Instance().RetainLookup(child);
  SWORDFS_LOG_DEBUG << "Create: ino=" << child.ino << " fh=" << handle->fh() << " name='" << name << "'";
  return Status::OK();
}

utils::Status VfsImpl::IoCtl(fuse_ino_t ino, unsigned int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
                             const void *in_buf, size_t in_bufsz, size_t out_bufsz, IoCtlReply *reply) {
  if (reply == nullptr) {
    return Status::InvalidArgument("ioctl reply output is null");
  }
  *reply = {};
  (void)arg;
  (void)fi;
  (void)flags;
  if (!runtime::MountRuntimeBehavior::Instance().IoctlEnabled()) {
    return Status::NotTty("ioctl control surface is disabled");
  }

  switch (cmd) {
    case FS_IOC_GETFLAGS: {
      // Linux FUSE fileattr_get/set transports the legacy flags as an
      // unsigned int even though FS_IOC_{GET,SET}FLAGS encode `long` in the
      // public ioctl number on 64-bit architectures.
      constexpr size_t kSize = sizeof(uint32_t);
      if (out_bufsz < kSize) {
        reply->retry = true;
        reply->retry_out_size = kSize;
        return Status::OK();
      }
      SwordFsInode inode;
      auto status = ResolveLiveInode(ino, &inode);
      if (!status.ok()) {
        return status;
      }
      const uint32_t legacy_flags = ToLegacyInodeFlags(inode.attr.inode_flags);
      SetIoCtlOutput(legacy_flags, reply);
      return Status::OK();
    }
    case FS_IOC_SETFLAGS: {
      constexpr size_t kSize = sizeof(uint32_t);
      if (in_buf == nullptr || in_bufsz < kSize) {
        reply->retry = true;
        reply->retry_in_size = kSize;
        return Status::OK();
      }
      uint32_t legacy_flags = 0;
      std::memcpy(&legacy_flags, in_buf, sizeof(legacy_flags));
      constexpr uint32_t kSupported = FS_IMMUTABLE_FL | FS_APPEND_FL;
      if ((legacy_flags & ~kSupported) != 0) {
        return Status::OperationNotSupported("unsupported inode flag bits");
      }
      return SetLocalAwareInodeFlags(ino, FromLegacyInodeFlags(legacy_flags));
    }
    case FS_IOC_FSGETXATTR: {
      constexpr size_t kSize = sizeof(struct fsxattr);
      if (out_bufsz < kSize) {
        reply->retry = true;
        reply->retry_out_size = kSize;
        return Status::OK();
      }
      SwordFsInode inode;
      auto status = ResolveLiveInode(ino, &inode);
      if (!status.ok()) {
        return status;
      }
      struct fsxattr projected{};
      projected.fsx_xflags = ToFsXFlags(inode.attr.inode_flags);
      SetIoCtlOutput(projected, reply);
      return Status::OK();
    }
    case FS_IOC_FSSETXATTR: {
      constexpr size_t kSize = sizeof(struct fsxattr);
      if (in_buf == nullptr || in_bufsz < kSize) {
        reply->retry = true;
        reply->retry_in_size = kSize;
        return Status::OK();
      }
      struct fsxattr requested{};
      std::memcpy(&requested, in_buf, sizeof(requested));
      constexpr uint32_t kSupported = FS_XFLAG_IMMUTABLE | FS_XFLAG_APPEND;
      bool reserved_nonzero = false;
      for (const auto byte : requested.fsx_pad) {
        reserved_nonzero = reserved_nonzero || byte != 0;
      }
      if ((requested.fsx_xflags & ~kSupported) != 0 || requested.fsx_extsize != 0 || requested.fsx_nextents != 0 ||
          requested.fsx_projid != 0 || requested.fsx_cowextsize != 0 || reserved_nonzero) {
        return Status::OperationNotSupported("unsupported fsxattr fields");
      }
      return SetLocalAwareInodeFlags(ino, FromFsXFlags(requested.fsx_xflags));
    }
    default:
      return Status::NotTty("unsupported ioctl command");
  }
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

utils::Status VfsImpl::StatX(fuse_ino_t ino, int flags, int mask, struct fuse_file_info *fi, struct statx *attr) {
  (void)flags;
  (void)mask;
  (void)fi;
  SwordFsInode inode;
  Status status = ResolveLiveInode(ino, &inode);
  if (status.ok()) {
    inode.attr.ToStatX(attr);
  }
  return status;
}

}  // namespace swordfs::vfs
