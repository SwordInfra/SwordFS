// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <exception>
#include <utility>

#include <sw/redis++/redis++.h>

#include "metadata/redis/RedisMetaStore.hpp"

namespace swordfs::metadata {
namespace {

Status NotImplemented() {
  return Status::NotSupported("Redis metadata operations are not implemented yet");
}

}  // namespace

RedisMetaImpl::RedisMetaImpl(const RedisMetaConfig& config)
    : store_(std::make_unique<RedisMetaStore>(config)) {}

RedisMetaImpl::~RedisMetaImpl() = default;

utils::Status RedisMetaImpl::Create(const RedisMetaConfig& config,
                                    std::unique_ptr<RedisMetaImpl>* out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Redis metadata engine output is null");
  }
  try {
    auto impl = std::make_unique<RedisMetaImpl>(config);
    auto status = impl->store_->Ping();
    if (!status.ok()) {
      return status;
    }
    *out = std::move(impl);
    return utils::Status::OK();
  } catch (const sw::redis::Error& error) {
    return utils::Status::IOError("Redis metadata initialization failed: " +
                                  std::string(error.what()));
  } catch (const std::exception& error) {
    return utils::Status::IOError("Redis metadata initialization failed: " +
                                  std::string(error.what()));
  }
}

Limits RedisMetaImpl::GetLimits() {
  return {.max_name_length = 255, .max_free_inodes = SIZE_MAX};
}

Status RedisMetaImpl::Lookup(InodeID, std::string_view, InodeID*, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::GetAttr(InodeID, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::ReadDir(InodeID, std::vector<SwordFsEntry>*) { return NotImplemented(); }
Status RedisMetaImpl::Create(InodeID, std::string_view, mode_t, InodeID*, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::MkDir(InodeID, std::string_view, mode_t, InodeID*, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::Unlink(InodeID, std::string_view, nlink_t*) { return NotImplemented(); }
Status RedisMetaImpl::RmDir(InodeID, std::string_view) { return NotImplemented(); }
Status RedisMetaImpl::Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult*) { return NotImplemented(); }
Status RedisMetaImpl::SetAttr(InodeID, const struct stat*, SetAttrField, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::StatFs(struct statvfs*) { return NotImplemented(); }
Status RedisMetaImpl::Access(InodeID, int) { return NotImplemented(); }
Status RedisMetaImpl::Symlink(InodeID, std::string_view, const char*, InodeID*, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::Link(InodeID, InodeID, std::string_view, struct stat*) { return NotImplemented(); }
Status RedisMetaImpl::Readlink(InodeID, std::string*) { return NotImplemented(); }
Status RedisMetaImpl::Open(InodeID) { return NotImplemented(); }
Status RedisMetaImpl::ReclaimInode(InodeID) { return NotImplemented(); }
Status RedisMetaImpl::ListChunks(InodeID, std::vector<ChunkMeta>*) { return NotImplemented(); }
Status RedisMetaImpl::OpenDir(InodeID) { return NotImplemented(); }
Status RedisMetaImpl::AddChunk(InodeID, const ChunkMeta&) { return NotImplemented(); }
Status RedisMetaImpl::FindChunk(InodeID, ChunkIndex, ChunkMeta*) { return NotImplemented(); }
Status RedisMetaImpl::Truncate(InodeID, size_t) { return NotImplemented(); }

}  // namespace swordfs::metadata
