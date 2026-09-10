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
#include "metadata/redis/RedisMetaTxn.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

constexpr Limits kRedisLimits{.max_name_length = 255, .max_free_inodes = UINT64_MAX};

utils::Status RedisMetaImpl::CreateInstance(std::string_view meta_url, std::string_view volume_name,
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

const RegisterMetaEngine kRedisMetaEngine{"redis", RedisMetaImpl::CreateInstance};

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
  return kRedisLimits;
}

Status RedisMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  return ops_.LookupEntry(parent_ino, name, out);
}

Status RedisMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  return ops_.GetInode(ino, out);
}

void RedisMetaImpl::UpdateAtimeBestEffort(InodeID ino) {
  auto status = ops_.TouchInode(ino, SetAttrField::kAtime);
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "failed to update atime for inode " << ino << ": " << status.message();
  }
}

Status RedisMetaImpl::OpenDir(InodeID ino, DirIteratorPtr *iterator) {
  if (iterator == nullptr) {
    return Status::InvalidArgument("directory iterator output is null");
  }
  SwordFsInode dir;
  auto status = ops_.GetInode(ino, &dir);
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

  UpdateAtimeBestEffort(ino);
  return Status::OK();
}

Status RedisMetaImpl::Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  if (name.size() > kRedisLimits.max_name_length) {
    return Status::NameTooLong("file name exceeds maximum length");
  }
  return CreateNode(parent_ino, name, S_IFREG | (mode & 0777u), out);
}

Status RedisMetaImpl::MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  if (name.size() > kRedisLimits.max_name_length) {
    return Status::NameTooLong("directory name exceeds maximum length");
  }
  return CreateNode(parent_ino, name, S_IFDIR | (mode & 0777u), out);
}

Status RedisMetaImpl::CreateNode(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  InodeID child_ino;
  // Allocate the inode ID before the metadata transaction because the ID is
  // needed to build the child record and Redis keys before EXEC. IDs only need
  // to be unique, not gap-free; a failed operation may leave a gap, while
  // transaction retries reuse the same reserved ID.
  auto status = ops_.AllocateInode(&child_ino);
  if (!status.ok()) {
    return status;
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  status = ops_.Transact([&](RedisMetaTxn &txn) {
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
    SwordFsAttr attr(child_ino, mode, ctx.uid, parent.attr.gid);
    child = SwordFsInode(child_ino, attr, parent_ino);
    return txn.AddEntry(parent_ino, name, child, &parent);
  });
  if (status.ok() && out != nullptr) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Unlink(InodeID parent_ino, std::string_view name, UnlinkResult *result) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  UnlinkResult unlink_result;
  auto status = ops_.Transact([&](RedisMetaTxn &txn) {
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
    status = txn.LookupEntry(parent, name, &child);
    if (!status.ok()) {
      return status;
    }
    if (child.IsDir()) {
      return Status::InvalidArgument("cannot unlink directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    return txn.UnlinkFile(parent_ino, name, &parent, &child, &unlink_result);
  });
  if (status.ok() && result != nullptr) {
    *result = unlink_result;
  }
  return status;
}

Status RedisMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return ops_.Transact([&](RedisMetaTxn &txn) {
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
    status = txn.LookupEntry(parent, name, &child);
    if (!status.ok()) {
      return status;
    }
    if (!child.IsDir()) {
      return Status::NotDirectory("not a directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    return txn.RemoveDirectory(parent_ino, name, &parent, child);
  });
}

Status RedisMetaImpl::Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                             std::string_view new_name, RenameFlag flags, RenameResult *result) {
  if (result != nullptr) {
    *result = {};
  }
  if (old_name.size() > kRedisLimits.max_name_length || new_name.size() > kRedisLimits.max_name_length) {
    return Status::NameTooLong("file name exceeds maximum length");
  }
  if (old_name == "." || old_name == ".." || new_name == "." || new_name == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  const uint32_t flag_bits = static_cast<uint32_t>(flags);
  const uint32_t supported_flags =
      static_cast<uint32_t>(RenameFlag::kNoReplace) | static_cast<uint32_t>(RenameFlag::kExchange);
  if ((flag_bits & ~supported_flags) != 0) {
    return Status::InvalidArgument("unsupported rename flags");
  }
  const bool no_replace = HasRenameFlag(flags, RenameFlag::kNoReplace);
  const bool exchange = HasRenameFlag(flags, RenameFlag::kExchange);
  if (no_replace && exchange) {
    return Status::InvalidArgument("RENAME_NOREPLACE and RENAME_EXCHANGE cannot be combined");
  }

  const auto ctx = folly::fibers::local<SwordFsContext>();
  RenameResult rename_result;
  auto status = ops_.Transact([&](RedisMetaTxn &txn) {
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
    status = txn.LookupEntry(old_parent, old_name, &source);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.CheckStickyDelete(ctx.uid, source)) {
      return Status::Permission("sticky bit denied on source");
    }

    SwordFsInode target;
    status = txn.LookupEntry(*new_parent_ptr, new_name, &target);
    const bool target_exists = status.ok();
    if (!target_exists && !status.IsNotFound()) {
      return status;
    }
    if (no_replace && target_exists) {
      return Status::AlreadyExists("target entry exists");
    }
    if (target_exists && target.ino == source.ino) {
      return Status::OK();
    }
    if (target_exists && !new_parent_ptr->CheckStickyDelete(ctx.uid, target)) {
      return Status::Permission("sticky bit denied on target");
    }

    if (exchange) {
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }
      return txn.ExchangeEntries(old_parent_ino, old_name, new_parent_ino, new_name, &old_parent, new_parent_ptr,
                                 &source, &target);
    }

    return txn.MoveEntry(old_parent_ino, old_name, new_parent_ino, new_name, &old_parent, new_parent_ptr, &source,
                         target_exists ? &target : nullptr, !no_replace, result != nullptr ? &rename_result : nullptr);
  });
  if (status.ok() && result != nullptr) {
    *result = rename_result;
  }
  return status;
}

Status RedisMetaImpl::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  return ops_.SetAttr(ino, requested, fields, out);
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
  const auto limits = GetLimits();
  *stbuf = {};
  stbuf->name_max = limits.max_name_length;
  stbuf->fragment_size = 4096;
  stbuf->block_size = 4096;
  stbuf->blocks = 268435456;
  stbuf->blocks_free = stbuf->blocks_available = stbuf->blocks;
  stbuf->files = files;
  stbuf->files_free = limits.max_free_inodes;
  return Status::OK();
}

Status RedisMetaImpl::Access(InodeID ino, uint32_t mask) {
  const auto ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode inode;
  auto status = ops_.GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  return inode.CheckAccess(ctx.uid, ctx.gid, mask) ? Status::OK() : Status::Permission("access denied");
}

Status RedisMetaImpl::Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) {
  if (name.size() > kRedisLimits.max_name_length) {
    return Status::NameTooLong("symlink name exceeds maximum length");
  }
  InodeID child_ino;
  auto status = ops_.AllocateInode(&child_ino);
  if (!status.ok()) {
    return status;
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  status = ops_.Transact([&](RedisMetaTxn &txn) {
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
    SwordFsAttr attr(child_ino, S_IFLNK | 0777u, ctx.uid, parent.attr.gid);
    attr.size = link.size();
    child = SwordFsInode(child_ino, attr, parent_ino, std::string(link));
    return txn.AddEntry(parent_ino, name, child, &parent);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) {
  if (newname.size() > kRedisLimits.max_name_length) {
    return Status::NameTooLong("link name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode linked;
  auto status = ops_.Transact([&](RedisMetaTxn &txn) {
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
    status = txn.LinkExistingEntry(newparent_ino, newname, &parent, &inode);
    if (!status.ok()) {
      return status;
    }
    linked = inode;
    return Status::OK();
  });
  if (status.ok() && out != nullptr) {
    *out = linked;
  }
  return status;
}

Status RedisMetaImpl::Readlink(InodeID ino, std::string *target) {
  if (!target) {
    return Status::InvalidArgument("Readlink output is null");
  }
  SwordFsInode inode;
  auto status = ops_.GetInode(ino, &inode);
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
  auto status = ops_.GetInode(ino, &inode);
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
  UpdateAtimeBestEffort(ino);
  return Status::OK();
}

Status RedisMetaImpl::ReclaimInode(InodeID ino) {
  return ops_.ReclaimInode(ino);
}

Status RedisMetaImpl::VisitChunks(InodeID ino, const ChunkVisitorFn &visitor) {
  if (!visitor) {
    return Status::InvalidArgument("chunk visitor is null");
  }
  SwordFsInode inode;
  auto status = ops_.GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }

  return ops_.VisitChunks(ino, visitor);
}

Status RedisMetaImpl::AddChunk(InodeID ino, const SwordFsChunk &chunk) {
  return ops_.AddChunk(ino, chunk);
}

Status RedisMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  return ops_.FindChunk(ino, idx, chunk);
}

Status RedisMetaImpl::Truncate(InodeID ino, uint64_t size) {
  return ops_.Truncate(ino, size);
}

}  // namespace swordfs::metadata
