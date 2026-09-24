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
#include "utils/ExecutionDomain.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

constexpr Limits kRedisLimits{.max_name_length = kMaxNameLength, .max_free_inodes = UINT64_MAX};

utils::Status RedisMetaImpl::CreateInstance(std::string_view meta_url, std::string_view volume_name,
                                            std::unique_ptr<IMetaEngine> *out) {
  utils::ExpectInThreadDomain();
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

Status RedisMetaImpl::BindChunkOverwriteStrategy(const chunk::IChunkOverwriteStrategy *strategy) {
  utils::ExpectInThreadDomain();
  return ops_.BindChunkOverwriteStrategy(strategy);
}

utils::Status RedisMetaImpl::Initialize() {
  utils::ExpectInThreadDomain();
  try {
    return ops_.Initialize();
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

utils::Status RedisMetaImpl::FormatVolume(const SwordFsVolume &config) {
  utils::ExpectInThreadDomain();
  return ops_.FormatVolume(config);
}

utils::Status RedisMetaImpl::LoadVolume(SwordFsVolume *config) {
  utils::ExpectInThreadDomain();
  return ops_.LoadVolume(config);
}

Limits RedisMetaImpl::GetLimits() const {
  return kRedisLimits;
}

Status RedisMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  return ops_.LookupEntry(parent_ino, name, out);
}

Status RedisMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  return ops_.GetInode(ino, out);
}

Status RedisMetaImpl::GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) {
  utils::ExpectInFiberDomain();
  return ops_.GetInodes(inode_ids, out);
}

void RedisMetaImpl::UpdateAtimeBestEffort(InodeID ino) {
  auto status = ops_.TouchInode(ino, SetAttrField::kAtime);
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "failed to update atime for inode " << ino << ": " << status.message();
  }
}

Status RedisMetaImpl::OpenDir(InodeID ino, DirIteratorPtr *iterator) {
  utils::ExpectInFiberDomain();
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
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  return CreateNode(parent_ino, name, S_IFREG | (mode & 0777u), 0, out);
}

Status RedisMetaImpl::MkNod(InodeID parent_ino, std::string_view name, uint32_t mode, uint64_t rdev,
                            SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  if (auto status = ValidateMknodMode(mode); !status.ok()) {
    return status;
  }
  return CreateNode(parent_ino, name, mode, NormalizeMknodRdev(mode, rdev), out);
}

Status RedisMetaImpl::MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  return CreateNode(parent_ino, name, S_IFDIR | (mode & 0777u), 0, out);
}

Status RedisMetaImpl::CreateNode(InodeID parent_ino, std::string_view name, uint32_t mode, uint64_t rdev,
                                 SwordFsInode *out) {
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
  status = ops_.TransactFromFiber([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    const auto inheritance = ResolveCreateInheritance(ctx.gid, parent.attr, mode);
    SwordFsAttr attr(child_ino, inheritance.mode, ctx.uid, inheritance.gid);
    attr.rdev = rdev;
    child = SwordFsInode(child_ino, attr, parent_ino);
    return txn.AddEntry(parent_ino, name, child, &parent);
  });
  if (status.ok() && out != nullptr) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Unlink(InodeID parent_ino, std::string_view name) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  auto status = ops_.TransactFromFiber([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
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
    return txn.UnlinkFile(parent_ino, name, &parent, &child);
  });
  return status;
}

Status RedisMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return ops_.TransactFromFiber([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
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
                             std::string_view new_name, RenameFlag flags) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(old_name); !status.ok()) {
    return status;
  }
  if (auto status = ValidateNameComponent(new_name); !status.ok()) {
    return status;
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
  auto status = ops_.TransactFromFiber([&](RedisMetaTxn &txn) {
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
                         target_exists ? &target : nullptr, !no_replace);
  });
  return status;
}

Status RedisMetaImpl::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  return ops_.SetAttr(ino, requested, fields, out);
}

Status RedisMetaImpl::StatFs(SwordFsStatFs *stbuf) {
  utils::ExpectInFiberDomain();
  if (!stbuf) {
    return Status::InvalidArgument("statfs output is null");
  }
  const auto limits = GetLimits();
  *stbuf = {};
  stbuf->name_max = limits.max_name_length;
  stbuf->fragment_size = 4096;
  stbuf->block_size = 4096;
  stbuf->blocks = 268435456;
  stbuf->blocks_free = stbuf->blocks_available = stbuf->blocks;
  // There is no inode quota. Do not expose advisory lifecycle accounting as
  // an exact filesystem capacity or make statfs depend on its integrity.
  stbuf->files = limits.max_free_inodes;
  stbuf->files_free = stbuf->files;
  return Status::OK();
}

Status RedisMetaImpl::Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  InodeID child_ino;
  auto status = ops_.AllocateInode(&child_ino);
  if (!status.ok()) {
    return status;
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  SwordFsInode child;
  status = ops_.TransactFromFiber([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    auto status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    const auto inheritance = ResolveCreateInheritance(ctx.gid, parent.attr, S_IFLNK | 0777u);
    SwordFsAttr attr(child_ino, inheritance.mode, ctx.uid, inheritance.gid);
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
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(newname); !status.ok()) {
    return status;
  }
  SwordFsInode linked;
  auto status = ops_.TransactFromFiber([&](RedisMetaTxn &txn) {
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
  utils::ExpectInFiberDomain();
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

Status RedisMetaImpl::Open(InodeID ino, uint64_t *size) {
  utils::ExpectInFiberDomain();
  SwordFsInode inode;
  auto status = ops_.GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return Status::NotDirectory("not a regular file");
  }
  if (size != nullptr) {
    *size = inode.attr.size;
  }
  UpdateAtimeBestEffort(ino);
  return Status::OK();
}

Status RedisMetaImpl::PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) {
  utils::ExpectInFiberDomain();
  if (work == nullptr) {
    return Status::InvalidArgument("reclaim work output is null");
  }
  return ops_.PrepareReclaim(ino, work);
}

Status RedisMetaImpl::CompleteReclaim(InodeID ino) {
  utils::ExpectInFiberDomain();
  return ops_.CompleteReclaim(ino);
}

Status RedisMetaImpl::VisitOrphanCandidates(const InodeVisitorFn &visitor) {
  utils::ExpectInFiberDomain();
  return ops_.VisitOrphanCandidates(visitor);
}

Status RedisMetaImpl::VisitPendingReclaims(const ReclaimVisitorFn &visitor) {
  utils::ExpectInFiberDomain();
  return ops_.VisitPendingReclaims(visitor);
}

Status RedisMetaImpl::VisitPendingDeletesBatch(size_t max_items, const PendingDeleteVisitorFn &visitor,
                                               bool *has_more) {
  utils::ExpectInFiberDomain();
  return ops_.VisitPendingDeletesBatch(max_items, visitor, has_more);
}

Status RedisMetaImpl::CompletePendingDelete(std::string_view key) {
  utils::ExpectInFiberDomain();
  return ops_.CompletePendingDelete(key);
}

Status RedisMetaImpl::AllocateChunkRevision(ChunkRevision *revision) {
  return ops_.AllocateChunkRevision(revision);
}

Status RedisMetaImpl::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                                  const SwordFsChunk &replacement) {
  return CommitChunk(ino, expected, replacement, {});
}

Status RedisMetaImpl::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                                  const SwordFsChunk &replacement, const ChunkPublishIntent &intent) {
  utils::ExpectInFiberDomain();
  return ops_.CommitChunk(ino, expected, replacement, intent);
}

Status RedisMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  utils::ExpectInFiberDomain();
  return ops_.FindChunk(ino, idx, chunk);
}

Status RedisMetaImpl::LoadChunkView(InodeID ino, ChunkIndex idx, ChunkView *out) {
  utils::ExpectInFiberDomain();
  return ops_.LoadChunkView(ino, idx, out);
}

Status RedisMetaImpl::Truncate(InodeID ino, uint64_t size) {
  utils::ExpectInFiberDomain();
  return ops_.Truncate(ino, size);
}

}  // namespace swordfs::metadata
