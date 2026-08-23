// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>

#include "metadata/IMetaEngine.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::metadata {

class RedisMetaStore;

// Phase 0 Redis metadata engine. It establishes the Redis connection and
// transaction infrastructure; metadata schema and IMetaEngine operations are
// added by subsequent issues.
class RedisMetaImpl : public IMetaEngine {
 public:
  explicit RedisMetaImpl(const RedisMetaConfig& config);
  ~RedisMetaImpl() override;

  RedisMetaImpl(const RedisMetaImpl&) = delete;
  RedisMetaImpl& operator=(const RedisMetaImpl&) = delete;

  static utils::Status Create(const RedisMetaConfig& config,
                              std::unique_ptr<RedisMetaImpl>* out);
  static Limits GetLimits();

  Status Lookup(InodeID parent_ino, std::string_view name,
                SwordFsInode* out) override;
  Status GetInode(InodeID ino, SwordFsInode* out) override;
  Status ReadDir(InodeID ino, std::vector<SwordFsEntry>* entries) override;
  Status Create(InodeID parent_ino, std::string_view name, mode_t mode,
                SwordFsInode* out) override;
  Status MkDir(InodeID parent_ino, std::string_view name, mode_t mode,
               SwordFsInode* out) override;
  Status Unlink(InodeID parent_ino, std::string_view name,
                nlink_t* post_nlink) override;
  Status RmDir(InodeID parent_ino, std::string_view name) override;
  Status Rename(InodeID old_parent_ino, std::string_view old_name,
                InodeID new_parent_ino, std::string_view new_name,
                RenameFlag flags, RenameResult* result) override;
  Status SetAttr(InodeID ino, const struct stat* attr, SetAttrField fields,
                 SwordFsInode* out) override;
  Status StatFs(struct statvfs* stbuf) override;
  Status Access(InodeID ino, int mask) override;
  Status Symlink(InodeID parent_ino, std::string_view name, const char* link,
                 SwordFsInode* out) override;
  Status Link(InodeID ino, InodeID newparent_ino, std::string_view newname,
              SwordFsInode* out) override;
  Status Readlink(InodeID ino, std::string* target) override;
  Status Open(InodeID ino) override;
  Status ReclaimInode(InodeID ino) override;
  Status ListChunks(InodeID ino, std::vector<ChunkMeta>* out) override;
  Status OpenDir(InodeID ino) override;
  Status AddChunk(InodeID ino, const ChunkMeta& cm) override;
  Status FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta* cm) override;
  Status Truncate(InodeID ino, size_t size) override;

 private:
  std::unique_ptr<RedisMetaStore> store_;
};

}  // namespace swordfs::metadata
