// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <sys/stat.h>

#include <algorithm>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

utils::Status RedisMetaImpl::Create(std::string_view meta_url, std::string_view volume_name,
                                    std::unique_ptr<IMetaEngine> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("metadata engine output is null");
  }

  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl(meta_url, &config);
  if (!status.ok()) {
    return status;
  }

  try {
    auto redis = std::make_unique<RedisMetaImpl>(config, volume_name);
    *out = std::move(redis);
    return utils::Status::OK();
  } catch (const std::invalid_argument &error) {
    return utils::Status::InvalidArgument(error.what());
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

const RegisterMetaEngine kRedisMetaEngine{"redis", RedisMetaImpl::Create};

RedisMetaImpl::RedisMetaImpl(const RedisMetaConfig &config, std::string_view volume_name) : ops_(config, volume_name) {
}

RedisMetaImpl::~RedisMetaImpl() = default;

utils::Status RedisMetaImpl::Initialize() {
  try {
    return ops_.Initialize();
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

utils::Status RedisMetaImpl::FormatVolume(const SwordFsVolume &config) {
  return ops_.FormatVolume(config);
}

utils::Status RedisMetaImpl::LoadVolume(SwordFsVolume *config) {
  return ops_.LoadVolume(config);
}

Limits RedisMetaImpl::GetLimits() const {
  return {.max_name_length = 255, .max_free_inodes = UINT64_MAX};
}

Status RedisMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("Lookup output is null");
  }
  return ops_.Transact([&](RedisMetaOps::Txn &txn) { return txn.LookupEntry(parent_ino, name, out); });
}

Status RedisMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  return ops_.GetInode(ino, out);
}

Status RedisMetaImpl::UpdateAtimeBestEffort(InodeID ino) {
  auto status = ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode inode;
    auto status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }
    inode.Touch(SetAttrField::kAtime);
    return txn.SetInode(inode);
  });
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "failed to update atime for inode " << ino << ": " << status.message();
  }
  return Status::OK();
}

Status RedisMetaImpl::OpenDir(InodeID ino, DirIteratorPtr *iterator) {
  if (iterator == nullptr) {
    return Status::InvalidArgument("directory iterator output is null");
  }
  SwordFsInode dir;
  auto status = GetInode(ino, &dir);
  if (!status.ok()) {
    return status;
  }
  if (!dir.IsDir()) {
    return Status::NotDirectory("not a directory");
  }

  std::vector<SwordFsEntry> prefix_entries{
      {".", DT_DIR, ino},
      {"..", DT_DIR, dir.parent_ino},
  };
  status = ops_.CreateDirIterator(ino, std::move(prefix_entries), iterator);
  if (!status.ok()) {
    return status;
  }

  status = UpdateAtimeBestEffort(ino);
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "OpenDir: failed to update atime for ino " << ino << ": " << status.message();
  }
  return Status::OK();
}

Status RedisMetaImpl::Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  if (name.size() > 255) {
    return Status::NameTooLong("file name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  InodeID child_ino;
  auto status = ops_.AllocateInode(&child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    bool exists = false;
    status = txn.EntryExists(parent_ino, name, &exists);
    if (!status.ok()) {
      return status;
    }
    if (exists) {
      return Status::AlreadyExists("entry already exists");
    }
    SwordFsAttr attr(child_ino, S_IFREG | (mode & 0777u));
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    child = SwordFsInode(child_ino, attr, parent_ino);
    status = txn.InsertInode(child);
    if (!status.ok()) {
      return status;
    }
    status = txn.LinkEntry(parent_ino, name, child, &parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.SetInode(parent);
    if (!status.ok()) {
      return status;
    }
    return txn.AdjustInodeCount(1);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  if (name.size() > 255) {
    return Status::NameTooLong("directory name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  InodeID child_ino;
  auto status = ops_.AllocateInode(&child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    bool exists = false;
    status = txn.EntryExists(parent_ino, name, &exists);
    if (!status.ok()) {
      return status;
    }
    if (exists) {
      return Status::AlreadyExists("entry already exists");
    }
    SwordFsAttr attr(child_ino, S_IFDIR | (mode & 0777u));
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    child = SwordFsInode(child_ino, attr, parent_ino);
    status = txn.InsertInode(child);
    if (!status.ok()) {
      return status;
    }
    status = txn.LinkEntry(parent_ino, name, child, &parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.SetInode(parent);
    if (!status.ok()) {
      return status;
    }
    return txn.AdjustInodeCount(1);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Unlink(InodeID parent_ino, std::string_view name, uint64_t *post_nlink) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    SwordFsInode child;
    status = txn.LookupEntry(parent_ino, name, &child);
    if (!status.ok()) {
      return status;
    }
    if (child.IsDir()) {
      return Status::InvalidArgument("cannot unlink directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    status = txn.UnlinkEntry(parent_ino, name, child, &parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.AdjustNlink(&child, -1, post_nlink);
    if (!status.ok()) {
      return status;
    }
    child.Touch(SetAttrField::kCtime);
    status = txn.SetInode(parent);
    if (!status.ok()) {
      return status;
    }
    return txn.SetInode(child);
  });
}

Status RedisMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    SwordFsInode child;
    status = txn.LookupEntry(parent_ino, name, &child);
    if (!status.ok()) {
      return status;
    }
    if (!child.IsDir()) {
      return Status::NotDirectory("not a directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    bool empty = false;
    status = txn.IsDirEmpty(child.ino, &empty);
    if (!status.ok()) {
      return status;
    }
    if (!empty) {
      return Status::NotEmpty("directory not empty");
    }
    status = txn.UnlinkEntry(parent_ino, name, child, &parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.DeleteDirectory(child.ino);
    if (!status.ok()) {
      return status;
    }
    status = txn.DeleteInode(child.ino);
    if (!status.ok()) {
      return status;
    }
    status = txn.SetInode(parent);
    if (!status.ok()) {
      return status;
    }
    return txn.AdjustInodeCount(-1);
  });
}

Status RedisMetaImpl::Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                             std::string_view new_name, RenameFlag flags, RenameResult *result) {
  if (result) {
    *result = {};
  }
  if (old_name.size() > 255 || new_name.size() > 255) {
    return Status::NameTooLong("file name exceeds maximum length");
  }
  if (old_name == "." || old_name == ".." || new_name == "." || new_name == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  const auto ctx = folly::fibers::local<SwordFsContext>();
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode old_parent;
    auto status = txn.LookupInode(old_parent_ino, &old_parent);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.IsDir()) {
      return Status::NotDirectory("old parent is not a directory");
    }
    SwordFsInode new_parent;
    SwordFsInode *new_parent_ptr = &old_parent;
    if (new_parent_ino != old_parent_ino) {
      status = txn.LookupInode(new_parent_ino, &new_parent);
      if (!status.ok()) {
        return status;
      }
      new_parent_ptr = &new_parent;
    }
    if (!new_parent_ptr->IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }
    if (!old_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on old parent");
    }
    if (!new_parent_ptr->CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }

    SwordFsInode source;
    status = txn.LookupEntry(old_parent_ino, old_name, &source);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.CheckStickyDelete(ctx.uid, source)) {
      return Status::Permission("sticky bit denied on source");
    }
    if (source.IsDir()) {
      bool cycle = false;
      status = txn.IsDescendantOf(source.ino, new_parent_ino, &cycle);
      if (!status.ok()) {
        return status;
      }
      if (cycle) {
        return Status::InvalidArgument("cannot move directory into its descendant");
      }
    }

    const bool no_replace = HasRenameFlag(flags, RenameFlag::kNoReplace);
    if (no_replace) {
      bool target_exists = false;
      status = txn.EntryExists(new_parent_ino, new_name, &target_exists);
      if (!status.ok()) {
        return status;
      }
      if (target_exists) {
        return Status::AlreadyExists("target entry exists");
      }
    }

    SwordFsInode target;
    bool target_exists = false;
    if (!no_replace || HasRenameFlag(flags, RenameFlag::kExchange)) {
      status = txn.LookupEntry(new_parent_ino, new_name, &target);
      target_exists = status.ok();
      if (!target_exists && !status.IsNotFound()) {
        return status;
      }
    }
    if (target_exists && target.ino == source.ino) {
      return Status::OK();
    }
    if (target_exists && !new_parent_ptr->CheckStickyDelete(ctx.uid, target)) {
      return Status::Permission("sticky bit denied on target");
    }

    if (HasRenameFlag(flags, RenameFlag::kExchange)) {
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }
      if (source.IsDir() != target.IsDir()) {
        return Status::InvalidArgument("cannot exchange directory with non-directory");
      }
      if (target.IsDir()) {
        bool cycle = false;
        status = txn.IsDescendantOf(target.ino, old_parent_ino, &cycle);
        if (!status.ok()) {
          return status;
        }
        if (cycle) {
          return Status::InvalidArgument("cannot exchange directory into its descendant");
        }
      }
      status = txn.ReplaceEntry(old_parent_ino, old_name, target, &old_parent);
      if (!status.ok()) {
        return status;
      }
      status = txn.ReplaceEntry(new_parent_ino, new_name, source, new_parent_ptr);
      if (!status.ok()) {
        return status;
      }
      source.parent_ino = new_parent_ino;
      target.parent_ino = old_parent_ino;
      source.Touch(SetAttrField::kCtime);
      target.Touch(SetAttrField::kCtime);
      status = txn.SetInode(old_parent);
      if (!status.ok()) {
        return status;
      }
      if (new_parent_ptr != &old_parent) {
        status = txn.SetInode(*new_parent_ptr);
        if (!status.ok()) {
          return status;
        }
      }
      status = txn.SetInode(source);
      if (!status.ok()) {
        return status;
      }
      return txn.SetInode(target);
    }

    if (target_exists) {
      if (source.IsDir() != target.IsDir()) {
        return source.IsDir() ? Status::NotDirectory("target is not a directory")
                              : Status::IsDirectory("target is a directory");
      }
      if (target.IsDir()) {
        bool empty = false;
        status = txn.IsDirEmpty(target.ino, &empty);
        if (!status.ok()) {
          return status;
        }
        if (!empty) {
          return Status::NotEmpty("target directory not empty");
        }
      }
      status = txn.UnlinkEntry(new_parent_ino, new_name, target, new_parent_ptr);
      if (!status.ok()) {
        return status;
      }
      if (target.IsDir()) {
        status = txn.DeleteDirectory(target.ino);
        if (!status.ok()) {
          return status;
        }
        status = txn.DeleteInode(target.ino);
        if (!status.ok()) {
          return status;
        }
        status = txn.AdjustInodeCount(-1);
        if (!status.ok()) {
          return status;
        }
      } else {
        uint64_t post_nlink = 0;
        status = txn.AdjustNlink(&target, -1, &post_nlink);
        if (!status.ok()) {
          return status;
        }
        target.Touch(SetAttrField::kCtime);
        if (result != nullptr) {
          result->overwritten_ino = target.ino;
          result->overwritten_post_nlink = post_nlink;
        }
      }
    }

    status = txn.UnlinkEntry(old_parent_ino, old_name, source, &old_parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.LinkEntry(new_parent_ino, new_name, source, new_parent_ptr);
    if (!status.ok()) {
      return status;
    }
    source.parent_ino = new_parent_ino;
    source.Touch(SetAttrField::kCtime);
    status = txn.SetInode(old_parent);
    if (!status.ok()) {
      return status;
    }
    if (new_parent_ptr != &old_parent) {
      status = txn.SetInode(*new_parent_ptr);
      if (!status.ok()) {
        return status;
      }
    }
    if (target_exists && !target.IsDir()) {
      status = txn.SetInode(target);
      if (!status.ok()) {
        return status;
      }
    }
    return txn.SetInode(source);
  });
}

Status RedisMetaImpl::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode inode;
    auto status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    SwordFsAttr attr = inode.attr;
    const uint64_t old_size = attr.size;
    const bool size_changed = HasSetAttrField(fields, SetAttrField::kSize) && old_size != requested.size;
    const bool owner_changed = (HasSetAttrField(fields, SetAttrField::kUid) && attr.uid != requested.uid) ||
                               (HasSetAttrField(fields, SetAttrField::kGid) && attr.gid != requested.gid);

    if (size_changed) {
      status = txn.TruncateChunks(ino, old_size, requested.size);
      if (!status.ok()) {
        return status;
      }
    }
    if (HasSetAttrField(fields, SetAttrField::kMode)) {
      attr.mode = (attr.mode & S_IFMT) | (requested.mode & 07777);
    }
    if (HasSetAttrField(fields, SetAttrField::kUid)) {
      attr.uid = requested.uid;
    }
    if (HasSetAttrField(fields, SetAttrField::kGid)) {
      attr.gid = requested.gid;
    }
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      attr.size = requested.size;
      if (!HasSetAttrField(fields, SetAttrField::kMtime) && !HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
        inode.attr = attr;
        inode.Touch(SetAttrField::kMtime);
        attr = inode.attr;
      }
    }
    if (HasSetAttrField(fields, SetAttrField::kAtime)) {
      attr.atime = requested.atime;
      attr.atime_nsec = requested.atime_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kMtime)) {
      attr.mtime = requested.mtime;
      attr.mtime_nsec = requested.mtime_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
      inode.attr = attr;
      inode.Touch(SetAttrField::kAtime);
      attr = inode.attr;
    }
    if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
      inode.attr = attr;
      inode.Touch(SetAttrField::kMtime);
      attr = inode.attr;
    }
    if (HasSetAttrField(fields, SetAttrField::kCtime)) {
      attr.ctime = requested.ctime;
      attr.ctime_nsec = requested.ctime_nsec;
    }
    if (size_changed || owner_changed) {
      attr.KillSUID();
    }
    if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
      inode.attr = attr;
      inode.Touch(SetAttrField::kCtime);
      attr = inode.attr;
    }

    inode.attr = attr;
    status = txn.SetInode(inode);
    if (!status.ok()) {
      return status;
    }
    if (out != nullptr) {
      *out = inode;
    }
    return Status::OK();
  });
}

Status RedisMetaImpl::StatFs(SwordFsStatFs *stbuf) {
  if (!stbuf) {
    return Status::InvalidArgument("statfs output is null");
  }
  uint64_t files = 0;
  auto status = ops_.GetInodeCount(&files);
  if (!status.ok()) {
    return status;
  }
  *stbuf = {};
  stbuf->name_max = 255;
  stbuf->fragment_size = 4096;
  stbuf->block_size = 4096;
  stbuf->blocks = 268435456;
  stbuf->blocks_free = stbuf->blocks_available = stbuf->blocks;
  stbuf->files = files;
  stbuf->files_free = UINT64_MAX;
  return Status::OK();
}

Status RedisMetaImpl::Access(InodeID ino, uint32_t mask) {
  const auto ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode inode;
  auto status = GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  return inode.CheckAccess(ctx.uid, ctx.gid, mask) ? Status::OK() : Status::Permission("access denied");
}

Status RedisMetaImpl::Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) {
  if (name.size() > 255) {
    return Status::NameTooLong("symlink name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  InodeID child_ino;
  auto status = ops_.AllocateInode(&child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    bool exists = false;
    status = txn.EntryExists(parent_ino, name, &exists);
    if (!status.ok()) {
      return status;
    }
    if (exists) {
      return Status::AlreadyExists("entry already exists");
    }
    SwordFsAttr attr(child_ino, S_IFLNK | 0777u);
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    attr.size = link.size();
    child = SwordFsInode(child_ino, attr, parent_ino, std::string(link));
    status = txn.InsertInode(child);
    if (!status.ok()) {
      return status;
    }
    status = txn.LinkEntry(parent_ino, name, child, &parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.SetInode(parent);
    if (!status.ok()) {
      return status;
    }
    return txn.AdjustInodeCount(1);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) {
  if (newname.size() > 255) {
    return Status::NameTooLong("link name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode inode;
    auto status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }
    if (inode.IsDir()) {
      return Status::NotPermitted("cannot hard-link directory");
    }
    SwordFsInode parent;
    status = txn.LookupInode(newparent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    bool exists = false;
    status = txn.EntryExists(newparent_ino, newname, &exists);
    if (!status.ok()) {
      return status;
    }
    if (exists) {
      return Status::AlreadyExists("entry already exists");
    }
    status = txn.LinkEntry(newparent_ino, newname, inode, &parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.AdjustNlink(&inode, 1);
    if (!status.ok()) {
      return status;
    }
    inode.Touch(SetAttrField::kCtime);
    status = txn.SetInode(parent);
    if (!status.ok()) {
      return status;
    }
    status = txn.SetInode(inode);
    if (!status.ok()) {
      return status;
    }
    if (out != nullptr) {
      *out = inode;
    }
    return Status::OK();
  });
}

Status RedisMetaImpl::Readlink(InodeID ino, std::string *target) {
  if (!target) {
    return Status::InvalidArgument("Readlink output is null");
  }
  SwordFsInode inode;
  auto status = GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsSymlink()) {
    return Status::InvalidArgument("not a symbolic link");
  }
  *target = inode.symlink_target;
  return Status::OK();
}

Status RedisMetaImpl::Open(InodeID ino) {
  SwordFsInode inode;
  auto status = GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return Status::NotDirectory("not a regular file");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  if (!inode.CheckAccess(ctx.uid, ctx.gid, R_OK)) {
    return Status::Permission("access denied");
  }
  return UpdateAtimeBestEffort(ino);
}

Status RedisMetaImpl::ReclaimInode(InodeID ino) {
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode inode;
    auto status = txn.LookupInode(ino, &inode);
    if (status.IsNotFound()) {
      return Status::OK();
    }
    if (!status.ok()) {
      return status;
    }
    if (inode.attr.nlink != 0) {
      return Status::OK();
    }
    status = txn.DeleteChunks(ino);
    if (!status.ok()) {
      return status;
    }
    status = txn.DeleteInode(ino);
    if (!status.ok()) {
      return status;
    }
    return txn.AdjustInodeCount(-1);
  });
}

Status RedisMetaImpl::VisitChunks(InodeID ino, const ChunkVisitorFn &visitor) {
  if (!visitor) {
    return Status::InvalidArgument("chunk visitor is null");
  }
  SwordFsInode inode;
  auto status = GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }

  return ops_.VisitChunks(ino, visitor);
}

Status RedisMetaImpl::AddChunk(InodeID ino, const SwordFsChunk &chunk) {
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode inode;
    auto status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }
    if (!inode.IsRegular()) {
      return Status::InvalidArgument("not a regular file");
    }
    return txn.SetChunk(ino, chunk);
  });
}

Status RedisMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  return ops_.FindChunk(ino, idx, chunk);
}

Status RedisMetaImpl::Truncate(InodeID ino, uint64_t size) {
  return ops_.Transact([&](RedisMetaOps::Txn &txn) {
    SwordFsInode inode;
    auto status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }
    if (inode.attr.size == size) {
      return Status::OK();
    }
    status = txn.TruncateChunks(ino, inode.attr.size, size);
    if (!status.ok()) {
      return status;
    }
    inode.attr.size = size;
    inode.attr.KillSUID();
    inode.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    return txn.SetInode(inode);
  });
}

}  // namespace swordfs::metadata
