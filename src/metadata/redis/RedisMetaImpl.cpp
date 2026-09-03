// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <sw/redis++/redis++.h>
#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"
#include "metadata/redis/RedisDirIterator.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
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

RedisMetaImpl::RedisMetaImpl(const RedisMetaConfig &config, std::string_view volume_name)
    : client_(std::make_shared<RedisMetaClient>(config)), key_(config.db, volume_name) {
}

RedisMetaImpl::~RedisMetaImpl() = default;

utils::Status RedisMetaImpl::Initialize() {
  try {
    return client_->Ping();
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

utils::Status RedisMetaImpl::FormatVolume(const SwordFsVolume &config) {
  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0755);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  status = client_->Transact([&](RedisMetaTxn &txn) {
    std::string existing;
    auto status = txn.Get(key_.Format(), &existing);
    if (status.ok()) {
      return utils::Status::AlreadyExists("Redis metadata volume is already formatted");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    status = txn.Set(key_.Format(), config.SerializeTo());
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.NextIno(), std::to_string(kRootInodeId));
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.InodeCount(), "1");
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(kRootInodeId), root_value);
  });
  if (status.ok()) {
    chunk_size_ = config.chunk_size;
  }
  return status;
}

utils::Status RedisMetaImpl::LoadVolume(SwordFsVolume *config) {
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis volume config output is null");
  }

  std::string value;
  auto status = client_->Get(key_.Format(), &value);
  if (!status.ok()) {
    return status;
  }

  SwordFsVolume loaded;
  status = loaded.ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  if (loaded.name != config->name) {
    return utils::Status::Malformed("volume name does not match Redis metadata namespace");
  }

  chunk_size_ = loaded.chunk_size;
  *config = std::move(loaded);
  return utils::Status::OK();
}

Limits RedisMetaImpl::GetLimits() const {
  return {.max_name_length = 255, .max_free_inodes = UINT64_MAX};
}

Status RedisMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("Lookup output is null");
  }
  std::string value;
  auto status = client_->Get(key_.Inode(parent_ino), &value);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode parent;
  status = parent.ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  if (!parent.IsDir()) {
    return Status::NotDirectory("parent is not a directory");
  }
  std::string entry_value;
  status = client_->HGet(key_.Directory(parent_ino), name, &entry_value);
  if (!status.ok()) {
    return status;
  }
  SwordFsEntry entry;
  status = entry.ParseFrom(entry_value);
  if (!status.ok()) {
    return status;
  }
  status = client_->Get(key_.Inode(entry.ino), &entry_value);
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(entry_value);
}

Status RedisMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("GetInode output is null");
  }
  std::string value;
  auto status = client_->Get(key_.Inode(ino), &value);
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

Status RedisMetaImpl::UpdateAtimeBestEffort(InodeID ino) {
  auto status = client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    inode.Touch(SetAttrField::kAtime);
    status = inode.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(ino), value);
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
  *iterator = std::make_shared<RedisDirIterator>(client_, key_.Directory(ino), std::move(prefix_entries));

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
  auto status = client_->Incr(key_.NextIno(), &child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = parent.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    std::string ignored;
    status = txn.HGet(key_.Directory(parent_ino), name, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    SwordFsAttr attr(child_ino, S_IFREG | (mode & 0777u));
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    child = SwordFsInode(child_ino, attr, parent_ino);
    parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    std::string child_value;
    status = child.SerializeTo(&child_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), child_value);
    if (!status.ok()) {
      return status;
    }
    std::string dir_entry_value;
    status = SwordFsEntry{std::string(name), DT_REG, child_ino}.SerializeTo(&dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(parent_ino), name, dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = parent.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), 1);
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
  auto status = client_->Incr(key_.NextIno(), &child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = parent.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    std::string ignored;
    status = txn.HGet(key_.Directory(parent_ino), name, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    SwordFsAttr attr(child_ino, S_IFDIR | (mode & 0777u));
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    child = SwordFsInode(child_ino, attr, parent_ino);
    parent.attr.nlink++;
    parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    std::string child_value;
    status = child.SerializeTo(&child_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), child_value);
    if (!status.ok()) {
      return status;
    }
    std::string dir_entry_value;
    status = SwordFsEntry{std::string(name), DT_DIR, child_ino}.SerializeTo(&dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(parent_ino), name, dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = parent.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), 1);
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
  return client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = parent.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    std::string entry_value;
    status = txn.HGet(key_.Directory(parent_ino), name, &entry_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsEntry entry;
    status = entry.ParseFrom(entry_value);
    if (!status.ok()) {
      return status;
    }
    const InodeID child_ino = entry.ino;
    SwordFsInode child;
    status = txn.Get(key_.Inode(child_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = child.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (child.IsDir()) {
      return Status::InvalidArgument("cannot unlink directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    child.attr.nlink--;
    child.Touch(SetAttrField::kCtime);
    parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    status = txn.HDel(key_.Directory(parent_ino), name);
    if (!status.ok()) {
      return status;
    }
    status = parent.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    status = child.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), value);
    if (!status.ok()) {
      return status;
    }
    if (post_nlink) {
      *post_nlink = child.attr.nlink;
    }
    return Status::OK();
  });
}

Status RedisMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent, child;
    std::string value, entry_value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = parent.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    status = txn.HGet(key_.Directory(parent_ino), name, &entry_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsEntry entry;
    status = entry.ParseFrom(entry_value);
    if (!status.ok()) {
      return status;
    }
    const InodeID child_ino = entry.ino;
    status = txn.Get(key_.Inode(child_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = child.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!child.IsDir()) {
      return Status::NotDirectory("not a directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    uint64_t length;
    status = txn.HLen(key_.Directory(child_ino), &length);
    if (!status.ok()) {
      return status;
    }
    if (length != 0) {
      return Status::NotEmpty("directory not empty");
    }
    parent.attr.nlink--;
    parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    status = txn.HDel(key_.Directory(parent_ino), name);
    if (!status.ok()) {
      return status;
    }
    status = txn.Del(key_.Directory(child_ino));
    if (!status.ok()) {
      return status;
    }
    status = txn.Del(key_.Inode(child_ino));
    if (!status.ok()) {
      return status;
    }
    status = parent.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), -1);
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
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string old_parent_value, new_parent_value, source_value, target_value, source_entry_value, target_entry_value;
    auto status = txn.Get(key_.Inode(old_parent_ino), &old_parent_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode old_parent;
    status = old_parent.ParseFrom(old_parent_value);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.IsDir()) {
      return Status::NotDirectory("old parent is not a directory");
    }
    status = txn.Get(key_.Inode(new_parent_ino), &new_parent_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode new_parent;
    status = new_parent.ParseFrom(new_parent_value);
    if (!status.ok()) {
      return status;
    }
    if (!new_parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }
    if (!old_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on old parent");
    }
    if (!new_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }
    status = txn.HGet(key_.Directory(old_parent_ino), old_name, &source_entry_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsEntry source_entry;
    status = source_entry.ParseFrom(source_entry_value);
    if (!status.ok()) {
      return status;
    }
    const uint32_t source_type = source_entry.type;
    const InodeID source_ino = source_entry.ino;
    status = txn.Get(key_.Inode(source_ino), &source_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode source;
    status = source.ParseFrom(source_value);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.CheckStickyDelete(ctx.uid, source)) {
      return Status::Permission("sticky bit denied on source");
    }

    auto is_descendant = [&](InodeID ancestor_ino, InodeID candidate_ino, bool *result_out) -> Status {
      if (result_out == nullptr) {
        return Status::InvalidArgument("cycle check output is null");
      }
      *result_out = false;
      InodeID current_ino = candidate_ino;
      folly::F14FastSet<InodeID> visited;
      while (current_ino != 0) {
        if (!visited.insert(current_ino).second) {
          return Status::Malformed("directory parent cycle detected");
        }
        if (current_ino == ancestor_ino) {
          *result_out = true;
          return Status::OK();
        }
        std::string inode_value;
        auto status = txn.Get(key_.Inode(current_ino), &inode_value);
        if (!status.ok()) {
          return status;
        }
        SwordFsInode inode;
        status = inode.ParseFrom(inode_value);
        if (!status.ok()) {
          return status;
        }
        if (inode.parent_ino == current_ino) {
          break;
        }
        current_ino = inode.parent_ino;
      }
      return Status::OK();
    };

    if (source.IsDir()) {
      bool cycle = false;
      status = is_descendant(source_ino, new_parent_ino, &cycle);
      if (!status.ok()) {
        return status;
      }
      if (cycle) {
        return Status::InvalidArgument("cannot move directory into its descendant");
      }
    }

    status = txn.HGet(key_.Directory(new_parent_ino), new_name, &target_entry_value);
    const bool target_exists = status.ok();
    if (!target_exists && !status.IsNotFound()) {
      return status;
    }
    if (HasRenameFlag(flags, RenameFlag::kNoReplace) && target_exists) {
      return Status::AlreadyExists("target entry exists");
    }
    if (HasRenameFlag(flags, RenameFlag::kExchange)) {
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }
      SwordFsEntry target_entry;
      status = target_entry.ParseFrom(target_entry_value);
      if (!status.ok()) {
        return status;
      }
      const uint32_t target_type = target_entry.type;
      const InodeID target_ino = target_entry.ino;
      if (target_ino == source_ino) {
        return Status::OK();
      }
      SwordFsInode target;
      status = txn.Get(key_.Inode(target_ino), &target_value);
      if (!status.ok()) {
        return status;
      }
      status = target.ParseFrom(target_value);
      if (!status.ok()) {
        return status;
      }
      if (!new_parent.CheckStickyDelete(ctx.uid, target)) {
        return Status::Permission("sticky bit denied on target");
      }
      if (source.IsDir() != target.IsDir()) {
        return Status::InvalidArgument("cannot exchange directory with non-directory");
      }
      if (target.IsDir()) {
        bool cycle = false;
        status = is_descendant(target_ino, old_parent_ino, &cycle);
        if (!status.ok()) {
          return status;
        }
        if (cycle) {
          return Status::InvalidArgument("cannot exchange directory into its descendant");
        }
      }
      status = SwordFsEntry{std::string(old_name), target_type, target_ino}.SerializeTo(&target_entry_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.HSet(key_.Directory(old_parent_ino), old_name, target_entry_value);
      if (!status.ok()) {
        return status;
      }
      status = SwordFsEntry{std::string(new_name), source_type, source_ino}.SerializeTo(&source_entry_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.HSet(key_.Directory(new_parent_ino), new_name, source_entry_value);
      if (!status.ok()) {
        return status;
      }
      source.parent_ino = new_parent_ino;
      target.parent_ino = old_parent_ino;
      source.Touch(SetAttrField::kCtime);
      target.Touch(SetAttrField::kCtime);
      old_parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
      new_parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
      status = source.SerializeTo(&source_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(source_ino), source_value);
      if (!status.ok()) {
        return status;
      }
      status = target.SerializeTo(&target_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(target_ino), target_value);
      if (!status.ok()) {
        return status;
      }
      status = old_parent.SerializeTo(&old_parent_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(old_parent_ino), old_parent_value);
      if (!status.ok()) {
        return status;
      }
      if (new_parent_ino != old_parent_ino) {
        status = new_parent.SerializeTo(&new_parent_value);
        if (!status.ok()) {
          return status;
        }
        status = txn.Set(key_.Inode(new_parent_ino), new_parent_value);
      }
      return status;
    }
    if (target_exists) {
      SwordFsEntry target_entry;
      status = target_entry.ParseFrom(target_entry_value);
      if (!status.ok()) {
        return status;
      }
      const InodeID target_ino = target_entry.ino;
      if (target_ino == source_ino) {
        return Status::OK();
      }
      SwordFsInode target;
      status = txn.Get(key_.Inode(target_ino), &target_value);
      if (!status.ok()) {
        return status;
      }
      status = target.ParseFrom(target_value);
      if (!status.ok()) {
        return status;
      }
      if (!new_parent.CheckStickyDelete(ctx.uid, target)) {
        return Status::Permission("sticky bit denied on target");
      }
      if (source.IsDir() != target.IsDir()) {
        return source.IsDir() ? Status::NotDirectory("target is not a directory")
                              : Status::IsDirectory("target is a directory");
      }
      if (target.IsDir()) {
        uint64_t length;
        status = txn.HLen(key_.Directory(target_ino), &length);
        if (!status.ok()) {
          return status;
        }
        if (length != 0) {
          return Status::NotEmpty("target directory not empty");
        }
        status = txn.Del(key_.Directory(target_ino));
        if (!status.ok()) {
          return status;
        }
        status = txn.Del(key_.Inode(target_ino));
        if (!status.ok()) {
          return status;
        }
        status = txn.IncrBy(key_.InodeCount(), -1);
        if (!status.ok()) {
          return status;
        }
        // Removing the victim directory removes its ".." backlink from its
        // parent. Keep the copy that will be serialized below authoritative
        // when both parent inodes are the same.
        if (old_parent_ino == new_parent_ino) {
          old_parent.attr.nlink--;
        } else {
          new_parent.attr.nlink--;
        }
      } else {
        if (target.attr.nlink > 0) {
          target.attr.nlink--;
        }
        target.Touch(SetAttrField::kCtime);
        if (target.attr.nlink == 0) { /* retain orphan for VFS reclaim */
        }
        status = target.SerializeTo(&target_value);
        if (!status.ok()) {
          return status;
        }
        status = txn.Set(key_.Inode(target_ino), target_value);
        if (!status.ok()) {
          return status;
        }
        if (result) {
          result->overwritten_ino = target_ino;
          result->overwritten_post_nlink = target.attr.nlink;
        }
      }
    }
    status = txn.HDel(key_.Directory(old_parent_ino), old_name);
    if (!status.ok()) {
      return status;
    }
    status = SwordFsEntry{std::string(new_name), source_type, source_ino}.SerializeTo(&source_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(new_parent_ino), new_name, source_entry_value);
    if (!status.ok()) {
      return status;
    }
    if (source.IsDir()) {
      source.parent_ino = new_parent_ino;
      if (old_parent_ino != new_parent_ino) {
        old_parent.attr.nlink--;
        new_parent.attr.nlink++;
      }
    } else {
      source.parent_ino = new_parent_ino;
    }
    source.Touch(SetAttrField::kCtime);
    old_parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    new_parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    status = source.SerializeTo(&source_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(source_ino), source_value);
    if (!status.ok()) {
      return status;
    }
    status = old_parent.SerializeTo(&old_parent_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(old_parent_ino), old_parent_value);
    if (!status.ok()) {
      return status;
    }
    if (new_parent_ino != old_parent_ino) {
      status = new_parent.SerializeTo(&new_parent_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(new_parent_ino), new_parent_value);
    }
    return status;
  });
}

Status RedisMetaImpl::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    const uint64_t old_size = inode.attr.size;
    const bool size_changed = HasSetAttrField(fields, SetAttrField::kSize) && old_size != requested.size;
    const bool owner_changed = (HasSetAttrField(fields, SetAttrField::kUid) && inode.attr.uid != requested.uid) ||
                               (HasSetAttrField(fields, SetAttrField::kGid) && inode.attr.gid != requested.gid);
    const bool mtime_changed = size_changed && !HasSetAttrField(fields, SetAttrField::kMtime) &&
                               !HasSetAttrField(fields, SetAttrField::kMtimeNow);
    if (size_changed) {
      status = TruncateChunks(txn, ino, old_size, requested.size);
      if (!status.ok()) {
        return status;
      }
    }
    if (HasSetAttrField(fields, SetAttrField::kMode)) {
      inode.attr.mode = (inode.attr.mode & S_IFMT) | (requested.mode & 07777);
    }
    if (HasSetAttrField(fields, SetAttrField::kUid)) {
      inode.attr.uid = requested.uid;
    }
    if (HasSetAttrField(fields, SetAttrField::kGid)) {
      inode.attr.gid = requested.gid;
    }
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      inode.attr.size = requested.size;
      if (mtime_changed) {
        inode.Touch(SetAttrField::kMtime);
      }
    }
    if (HasSetAttrField(fields, SetAttrField::kAtime)) {
      inode.attr.atime = requested.atime;
      inode.attr.atime_nsec = requested.atime_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kMtime)) {
      inode.attr.mtime = requested.mtime;
      inode.attr.mtime_nsec = requested.mtime_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
      inode.Touch(SetAttrField::kAtime);
    }
    if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
      inode.Touch(SetAttrField::kMtime);
    }
    if (HasSetAttrField(fields, SetAttrField::kCtime)) {
      inode.attr.ctime = requested.ctime;
      inode.attr.ctime_nsec = requested.ctime_nsec;
    }
    if (size_changed || owner_changed) {
      inode.attr.KillSUID();
    }
    if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
      inode.Touch(SetAttrField::kCtime);
    }
    status = inode.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(ino), value);
    if (!status.ok()) {
      return status;
    }
    if (out) {
      *out = inode;
    }
    return Status::OK();
  });
}

Status RedisMetaImpl::StatFs(SwordFsStatFs *stbuf) {
  if (!stbuf) {
    return Status::InvalidArgument("statfs output is null");
  }
  std::string value;
  auto status = client_->Get(key_.InodeCount(), &value);
  if (!status.ok()) {
    return status;
  }
  uint64_t files = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), files);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return Status::IOError("invalid Redis inode count");
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
  std::string value;
  auto status = client_->Get(key_.Inode(ino), &value);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode inode;
  status = inode.ParseFrom(value);
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
  auto status = client_->Incr(key_.NextIno(), &child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = client_->Transact([&](RedisMetaTxn &txn) {
    std::string value, ignored;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode parent;
    status = parent.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    status = txn.HGet(key_.Directory(parent_ino), name, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    SwordFsAttr attr(child_ino, S_IFLNK | 0777u);
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    attr.size = link.size();
    child = SwordFsInode(child_ino, attr, parent_ino, std::string(link));
    parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    status = child.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), value);
    if (!status.ok()) {
      return status;
    }
    std::string dir_entry_value;
    status = SwordFsEntry{std::string(name), DT_LNK, child_ino}.SerializeTo(&dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(parent_ino), name, dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = parent.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), 1);
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
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value, ignored;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (inode.IsDir()) {
      return Status::NotPermitted("cannot hard-link directory");
    }
    status = txn.Get(key_.Inode(newparent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode parent;
    status = parent.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    status = txn.HGet(key_.Directory(newparent_ino), newname, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    inode.attr.nlink++;
    inode.Touch(SetAttrField::kCtime);
    parent.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    std::string dir_entry_value;
    status = SwordFsEntry{std::string(newname), ModeToDt(inode.attr.mode), ino}.SerializeTo(&dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(newparent_ino), newname, dir_entry_value);
    if (!status.ok()) {
      return status;
    }
    status = inode.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(ino), value);
    if (!status.ok()) {
      return status;
    }
    status = parent.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(newparent_ino), value);
    if (status.ok() && out) {
      *out = inode;
    }
    return status;
  });
}

Status RedisMetaImpl::Readlink(InodeID ino, std::string *target) {
  if (!target) {
    return Status::InvalidArgument("Readlink output is null");
  }
  std::string value;
  auto status = client_->Get(key_.Inode(ino), &value);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode inode;
  status = inode.ParseFrom(value);
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
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (status.IsNotFound()) {
      return Status::OK();
    }
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (inode.attr.nlink != 0) {
      return Status::OK();
    }
    status = txn.Del(key_.Chunk(ino));
    if (!status.ok()) {
      return status;
    }
    status = txn.Del(key_.Inode(ino));
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), -1);
  });
}

Status RedisMetaImpl::VisitChunks(InodeID ino, const ChunkVisitorFn &visitor) {
  if (!visitor) {
    return Status::InvalidArgument("chunk visitor is null");
  }
  std::string inode_value;
  auto status = client_->Get(key_.Inode(ino), &inode_value);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode inode;
  status = inode.ParseFrom(inode_value);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }

  constexpr size_t kScanBatchSize = 128;
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    status = client_->HScan(key_.Chunk(ino), cursor, kScanBatchSize, &values, &next_cursor);
    if (!status.ok()) {
      return status;
    }
    for (const auto &[field, chunk_value] : values) {
      (void)field;
      SwordFsChunk chunk;
      status = chunk.ParseFrom(chunk_value);
      if (!status.ok()) {
        return status;
      }
      status = visitor(chunk);
      if (!status.ok()) {
        return status;
      }
    }
    cursor = next_cursor;
  } while (cursor != 0);
  return Status::OK();
}

Status RedisMetaImpl::AddChunk(InodeID ino, const SwordFsChunk &chunk) {
  std::string chunk_value;
  auto status = chunk.SerializeTo(&chunk_value);
  if (!status.ok()) {
    return status;
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (!inode.IsRegular()) {
      return Status::InvalidArgument("not a regular file");
    }
    return txn.HSet(key_.Chunk(ino), std::to_string(chunk.index), chunk_value);
  });
}

Status RedisMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  if (!chunk) {
    return Status::InvalidArgument("FindChunk output is null");
  }
  std::string value;
  auto status = client_->HGet(key_.Chunk(ino), std::to_string(idx), &value);
  if (!status.ok()) {
    return status;
  }
  return chunk->ParseFrom(value);
}

Status RedisMetaImpl::TruncateChunks(RedisMetaTxn &txn, InodeID ino, uint64_t old_size, uint64_t new_size) {
  if (new_size >= old_size) {
    return Status::OK();
  }
  if (chunk_size_ == 0) {
    return Status::Internal("volume chunk size is not initialized");
  }

  const std::string chunk_key = key_.Chunk(ino);
  if (new_size == 0) {
    return txn.Del(chunk_key);
  }

  const ChunkIndex boundary_idx = static_cast<ChunkIndex>(new_size / chunk_size_);
  const uint64_t boundary_offset = new_size % chunk_size_;
  const ChunkIndex first_removed_idx = boundary_idx + (boundary_offset != 0 ? 1 : 0);
  const ChunkIndex old_chunk_count = static_cast<ChunkIndex>((old_size + chunk_size_ - 1) / chunk_size_);

  std::optional<std::string> boundary_value;
  if (boundary_offset != 0) {
    std::string value;
    auto status = txn.HGet(chunk_key, std::to_string(boundary_idx), &value);
    if (status.ok()) {
      SwordFsChunk chunk;
      status = chunk.ParseFrom(value);
      if (!status.ok()) {
        return status;
      }
      const uint64_t new_chunk_size = new_size - chunk.start_offset;
      if (chunk.size > new_chunk_size) {
        chunk.size = new_chunk_size;
        status = chunk.SerializeTo(&value);
        if (!status.ok()) {
          return status;
        }
        boundary_value = std::move(value);
      }
    } else if (!status.IsNotFound()) {
      return status;
    }
  }

  // RedisMetaTxn requires all reads before writes. Queue mutations only after
  // the optional boundary lookup has finished.
  if (boundary_value.has_value()) {
    auto status = txn.HSet(chunk_key, std::to_string(boundary_idx), *boundary_value);
    if (!status.ok()) {
      return status;
    }
  }
  // Chunk fields use their fixed-size chunk index as the hash field. The old
  // inode size bounds every valid index, and HDEL is harmless for sparse or
  // missing chunks, so truncation never needs a hash-wide scan.
  for (ChunkIndex idx = first_removed_idx; idx < old_chunk_count; ++idx) {
    auto status = txn.HDel(chunk_key, std::to_string(idx));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::OK();
}

Status RedisMetaImpl::Truncate(InodeID ino, uint64_t size) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    const uint64_t old_size = inode.attr.size;
    if (old_size == size) {
      return Status::OK();
    }
    status = TruncateChunks(txn, ino, old_size, size);
    if (!status.ok()) {
      return status;
    }
    inode.attr.size = size;
    inode.attr.KillSUID();
    inode.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
    status = inode.SerializeTo(&value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(ino), value);
  });
}

}  // namespace swordfs::metadata
