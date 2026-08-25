// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <sw/redis++/redis++.h>

#include <ctime>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"
#include "metadata/redis/RedisCodec.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"

namespace swordfs::metadata {
namespace {
utils::Status CreateRedisMetaEngine(std::string_view meta_url, std::string_view volume_name,
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
    if (volume_name.empty()) {
      return utils::Status::InvalidArgument("Redis metadata volume name is empty");
    }
    auto redis = std::make_unique<RedisMetaImpl>(config, volume_name);
    status = redis->Initialize();
    if (!status.ok()) {
      return status;
    }
    *out = std::move(redis);
    return utils::Status::OK();
  } catch (const std::invalid_argument &error) {
    return utils::Status::InvalidArgument(error.what());
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

RegisterMetaEngine kRedisMetaEngine{"redis", CreateRedisMetaEngine};
}  // namespace

RedisMetaImpl::RedisMetaImpl(const RedisMetaConfig &config, std::string_view volume_name)
    : client_(std::make_unique<RedisMetaClient>(config)), key_(config.db, volume_name) {
}

RedisMetaImpl::~RedisMetaImpl() = default;

utils::Status RedisMetaImpl::Initialize() {
  try {
    return client_->Ping();
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

utils::Status RedisMetaImpl::Format() {
  RedisFormat format;
  std::string format_value;
  auto status = RedisCodec::EncodeFormat(format, &format_value);
  if (!status.ok()) {
    return status;
  }

  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = MakeStat(S_IFDIR | 0777, ::time(nullptr));
  root.attr.st_ino = kRootInodeId;
  std::string root_value;
  status = RedisCodec::EncodeInode(root, &root_value);
  if (!status.ok()) {
    return status;
  }

  return client_->Transact([&](RedisMetaTxn &txn) {
    auto status = txn.Watch(key_.Format());
    if (!status.ok()) {
      return status;
    }
    std::optional<std::string> existing;
    status = txn.Get(key_.Format(), &existing);
    if (!status.ok()) {
      return status;
    }
    if (existing.has_value()) {
      RedisFormat existing_format;
      status = RedisCodec::DecodeFormat(*existing, &existing_format);
      if (!status.ok()) {
        return status;
      }
      return utils::Status::AlreadyExists("Redis metadata volume is already formatted");
    }
    status = txn.Set(key_.Format(), format_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.NextIno(), "2");
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(kRootInodeId), root_value);
  });
}

utils::Status RedisMetaImpl::Validate() {
  return client_->Transact([&](RedisMetaTxn &txn) {
    auto status = txn.Watch(key_.Format());
    if (!status.ok()) {
      return status;
    }
    std::optional<std::string> value;
    status = txn.Get(key_.Format(), &value);
    if (!status.ok()) {
      return status;
    }
    if (!value.has_value()) {
      return utils::Status::NotFound("Redis metadata volume is not formatted");
    }
    RedisFormat format;
    status = RedisCodec::DecodeFormat(*value, &format);
    if (!status.ok()) {
      return status;
    }
    std::optional<std::string> root_value;
    status = txn.Get(key_.Inode(kRootInodeId), &root_value);
    if (!status.ok()) {
      return status;
    }
    if (!root_value.has_value()) {
      return utils::Status::InvalidArgument("Redis metadata root inode is missing");
    }
    SwordFsInode root;
    status = RedisCodec::DecodeInode(*root_value, &root);
    if (!status.ok()) {
      return status;
    }
    if (root.ino != kRootInodeId || !root.IsDir()) {
      return utils::Status::InvalidArgument("Redis metadata root inode is invalid");
    }
    return utils::Status::OK();
  });
}

Limits RedisMetaImpl::GetLimits() const {
  return {.max_name_length = 255, .max_free_inodes = SIZE_MAX};
}

Status RedisMetaImpl::Lookup(InodeID, std::string_view, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::GetInode(InodeID, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::ReadDir(InodeID, std::vector<SwordFsEntry> *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Create(InodeID, std::string_view, mode_t, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::MkDir(InodeID, std::string_view, mode_t, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Unlink(InodeID, std::string_view, nlink_t *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::RmDir(InodeID, std::string_view) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::SetAttr(InodeID, const struct stat *, SetAttrField, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::StatFs(struct statvfs *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Access(InodeID, int) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Symlink(InodeID, std::string_view, const char *, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Link(InodeID, InodeID, std::string_view, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Readlink(InodeID, std::string *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Open(InodeID) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::ReclaimInode(InodeID) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::ListChunks(InodeID, std::vector<ChunkMeta> *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::OpenDir(InodeID) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::AddChunk(InodeID, const ChunkMeta &) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::FindChunk(InodeID, ChunkIndex, ChunkMeta *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Truncate(InodeID, size_t) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}

}  // namespace swordfs::metadata
