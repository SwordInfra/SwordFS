// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaOps.hpp"

#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "metadata/redis/RedisDirIterator.hpp"

namespace swordfs::metadata {

RedisMetaOps::RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name)
    : raw_(config), key_(config.db, volume_name), txn_(raw_, key_, chunk_size_) {
}

utils::Status RedisMetaOps::Initialize() {
  return raw_.Ping();
}

utils::Status RedisMetaOps::FormatVolume(const SwordFsVolume &config) {
  auto status = txn_.FormatVolume(config);
  if (status.ok()) {
    chunk_size_ = config.chunk_size;
  }
  return status;
}

utils::Status RedisMetaOps::LoadVolume(SwordFsVolume *config) {
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis volume config output is null");
  }

  std::string value;
  auto status = raw_.Get(key_.Format(), &value);
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

utils::Status RedisMetaOps::CreateDirIterator(InodeID ino, std::vector<SwordFsEntry> prefix_entries,
                                              std::shared_ptr<DirIterator> *iterator) {
  if (iterator == nullptr) {
    return utils::Status::InvalidArgument("directory iterator output is null");
  }
  *iterator = std::make_shared<RedisDirIterator>(raw_, key_.Directory(ino), std::move(prefix_entries));
  return utils::Status::OK();
}

utils::Status RedisMetaOps::GetInode(InodeID ino, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }
  std::string value;
  auto status = raw_.Get(key_.Inode(ino), &value);
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

utils::Status RedisMetaOps::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  if (chunk == nullptr) {
    return utils::Status::InvalidArgument("chunk output is null");
  }
  std::string value;
  auto status = raw_.HGet(key_.Chunk(ino), std::to_string(idx), &value);
  if (!status.ok()) {
    return status;
  }
  return chunk->ParseFrom(value);
}

utils::Status RedisMetaOps::VisitChunks(InodeID ino,
                                        const std::function<utils::Status(const SwordFsChunk &)> &visitor) {
  if (!visitor) {
    return utils::Status::InvalidArgument("chunk visitor is null");
  }

  constexpr size_t kScanBatchSize = 128;
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    auto status = raw_.HScan(key_.Chunk(ino), cursor, kScanBatchSize, &values, &next_cursor);
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
  return utils::Status::OK();
}

utils::Status RedisMetaOps::GetInodeCount(uint64_t *count) {
  if (count == nullptr) {
    return utils::Status::InvalidArgument("inode count output is null");
  }
  std::string value;
  auto status = raw_.Get(key_.InodeCount(), &value);
  if (!status.ok()) {
    return status;
  }
  uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return utils::Status::IOError("invalid Redis inode count");
  }
  *count = parsed;
  return utils::Status::OK();
}

utils::Status RedisMetaOps::AllocateInode(InodeID *ino) {
  if (ino == nullptr) {
    return utils::Status::InvalidArgument("inode id output is null");
  }
  return raw_.Incr(key_.NextIno(), ino);
}

utils::Status RedisMetaOps::Transact(const TransactFn &callback) {
  return txn_.Transact(callback);
}

utils::Status RedisMetaOps::LookupInode(RedisMetaOpsContext &ctx, InodeID ino, SwordFsInode *out) {
  return txn_.LookupInode(ctx, ino, out);
}

utils::Status RedisMetaOps::LookupEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                        SwordFsInode *out) {
  return txn_.LookupEntry(ctx, parent_ino, name, out);
}

utils::Status RedisMetaOps::EntryExists(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                        bool *exists) {
  return txn_.EntryExists(ctx, parent_ino, name, exists);
}

utils::Status RedisMetaOps::IsDirEmpty(RedisMetaOpsContext &ctx, InodeID ino, bool *empty) {
  return txn_.IsDirEmpty(ctx, ino, empty);
}

utils::Status RedisMetaOps::IsDescendantOf(RedisMetaOpsContext &ctx, InodeID ancestor_ino, InodeID child_ino,
                                           bool *result) {
  return txn_.IsDescendantOf(ctx, ancestor_ino, child_ino, result);
}

utils::Status RedisMetaOps::InsertInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode) {
  return txn_.InsertInode(ctx, inode);
}

utils::Status RedisMetaOps::SetInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode) {
  return txn_.SetInode(ctx, inode);
}

utils::Status RedisMetaOps::DeleteInode(RedisMetaOpsContext &ctx, InodeID ino) {
  return txn_.DeleteInode(ctx, ino);
}

utils::Status RedisMetaOps::AdjustNlink(RedisMetaOpsContext &ctx, SwordFsInode *inode, int delta, uint64_t *nlink) {
  return txn_.AdjustNlink(ctx, inode, delta, nlink);
}

utils::Status RedisMetaOps::LinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                      const SwordFsInode &child, SwordFsInode *parent) {
  return txn_.LinkEntry(ctx, parent_ino, name, child, parent);
}

utils::Status RedisMetaOps::UnlinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                        const SwordFsInode &target, SwordFsInode *parent) {
  return txn_.UnlinkEntry(ctx, parent_ino, name, target, parent);
}

utils::Status RedisMetaOps::ReplaceEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                         const SwordFsInode &child, SwordFsInode *parent) {
  return txn_.ReplaceEntry(ctx, parent_ino, name, child, parent);
}

utils::Status RedisMetaOps::DeleteDirectory(RedisMetaOpsContext &ctx, InodeID ino) {
  return txn_.DeleteDirectory(ctx, ino);
}

utils::Status RedisMetaOps::AdjustInodeCount(RedisMetaOpsContext &ctx, int64_t delta) {
  return txn_.AdjustInodeCount(ctx, delta);
}

utils::Status RedisMetaOps::SetChunk(RedisMetaOpsContext &ctx, InodeID ino, const SwordFsChunk &chunk) {
  return txn_.SetChunk(ctx, ino, chunk);
}

utils::Status RedisMetaOps::TruncateChunks(RedisMetaOpsContext &ctx, InodeID ino, uint64_t old_size,
                                           uint64_t new_size) {
  return txn_.TruncateChunks(ctx, ino, old_size, new_size);
}

utils::Status RedisMetaOps::DeleteChunks(RedisMetaOpsContext &ctx, InodeID ino) {
  return txn_.DeleteChunks(ctx, ino);
}

}  // namespace swordfs::metadata
