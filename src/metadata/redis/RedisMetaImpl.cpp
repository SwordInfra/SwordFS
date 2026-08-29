// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <sw/redis++/redis++.h>
#include <unistd.h>

#include <ctime>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"

namespace swordfs::metadata {
namespace {
using namespace redis;
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
    auto redis = std::make_unique<RedisMetaImpl>(config, volume_name);
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

utils::Status RedisMetaImpl::FormatVolume(const SwordFsVolume &config) {
  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0777);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  return client_->Transact([&](RedisMetaTxn &txn) {
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
    status = txn.Set(key_.NextIno(), std::to_string(kRootInodeId + 1));
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(kRootInodeId), root_value);
  });
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

  std::string root_value;
  status = client_->Get(key_.Inode(kRootInodeId), &root_value);
  if (!status.ok()) {
    return status;
  }

  SwordFsInode root;
  status = root.ParseFrom(root_value);
  if (!status.ok()) {
    return status;
  }
  if (root.ino != kRootInodeId || !root.IsDir()) {
    return utils::Status::InvalidArgument("Redis metadata root inode is invalid");
  }
  status = config->ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  return utils::Status::OK();
}

Limits RedisMetaImpl::GetLimits() const {
  return {.max_name_length = 255, .max_free_inodes = UINT64_MAX};
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
Status RedisMetaImpl::Create(InodeID, std::string_view, uint32_t, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Unlink(InodeID, std::string_view, uint64_t *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::RmDir(InodeID, std::string_view) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::StatFs(SwordFsStatFs *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Access(InodeID, uint32_t) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) {
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
Status RedisMetaImpl::ListChunks(InodeID, std::vector<SwordFsChunk> *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::OpenDir(InodeID) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::AddChunk(InodeID, const SwordFsChunk &) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::FindChunk(InodeID, ChunkIndex, SwordFsChunk *) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}
Status RedisMetaImpl::Truncate(InodeID, uint64_t) {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}

}  // namespace swordfs::metadata
