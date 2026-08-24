// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <sw/redis++/redis++.h>

#include <exception>
#include <utility>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/redis/RedisMetaClient.hpp"

namespace swordfs::metadata {
namespace {
RegisterMetaEngine kRedisMetaEngine{"redis"};
}  // namespace

RedisMetaImpl::RedisMetaImpl(const RedisMetaConfig &config) : client_(std::make_unique<RedisMetaClient>(config)) {
}

RedisMetaImpl::~RedisMetaImpl() = default;

utils::Status RedisMetaImpl::Initialize() {
  try {
    return client_->Ping();
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

Limits RedisMetaImpl::GetLimits() {
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
