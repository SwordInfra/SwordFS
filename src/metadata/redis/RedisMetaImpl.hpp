// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>

#include "metadata/DirIterator.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::metadata {

class RedisMetaClient;

// Redis metadata engine. Connection and transaction infrastructure is shared
// with the persistent schema/format layer; metadata operations are added by
// subsequent issues.
class RedisMetaImpl : public IMetaEngine {
 public:
  RedisMetaImpl(const RedisMetaConfig &config, std::string_view volume_name);
  ~RedisMetaImpl() override;

  RedisMetaImpl(const RedisMetaImpl &) = delete;
  RedisMetaImpl &operator=(const RedisMetaImpl &) = delete;

  utils::Status Initialize() override;
  utils::Status FormatVolume(const SwordFsVolume &config) override;
  utils::Status LoadVolume(SwordFsVolume *config) override;
  Limits GetLimits() const override;

  Status Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) override;
  Status GetInode(InodeID ino, SwordFsInode *out) override;
  Status ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) override;
  Status OpenDirIterator(InodeID ino, std::unique_ptr<IDirIterator> *out) override;
  Status Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) override;
  Status Unlink(InodeID parent_ino, std::string_view name, uint64_t *post_nlink) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino, std::string_view new_name,
                RenameFlag flags, RenameResult *result) override;
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override;
  Status StatFs(SwordFsStatFs *stbuf) override;
  Status Access(InodeID ino, uint32_t mask) override;
  Status Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) override;
  Status Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) override;
  Status Readlink(InodeID ino, std::string *target) override;
  Status Open(InodeID ino) override;
  Status ReclaimInode(InodeID ino) override;
  Status ListChunks(InodeID ino, std::vector<SwordFsChunk> *out) override;
  Status OpenDir(InodeID ino) override;
  Status AddChunk(InodeID ino, const SwordFsChunk &chunk) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) override;
  Status Truncate(InodeID ino, uint64_t size) override;

 private:
  std::shared_ptr<RedisMetaClient> client_;
  redis::RedisKey key_;
};

}  // namespace swordfs::metadata
