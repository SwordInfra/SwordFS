// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaOps.hpp"

#include <sys/stat.h>

#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "metadata/redis/RedisDirIterator.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"

namespace swordfs::metadata {

RedisMetaOps::RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name)
    : client_(std::make_shared<RedisMetaClient>(config)), key_(config.db, volume_name) {
}

utils::Status RedisMetaOps::Initialize() {
  return client_->Ping();
}

utils::Status RedisMetaOps::FormatVolume(const SwordFsVolume &config) {
  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0755);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  status = client_->Transact([&](RedisKvTxn &txn) {
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

utils::Status RedisMetaOps::LoadVolume(SwordFsVolume *config) {
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

utils::Status RedisMetaOps::CreateDirIterator(InodeID ino, std::vector<SwordFsEntry> prefix_entries,
                                              std::shared_ptr<DirIterator> *iterator) {
  if (iterator == nullptr) {
    return utils::Status::InvalidArgument("directory iterator output is null");
  }
  *iterator = std::make_shared<RedisDirIterator>(client_, key_.Directory(ino), std::move(prefix_entries));
  return utils::Status::OK();
}

utils::Status RedisMetaOps::GetInode(InodeID ino, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }
  std::string value;
  auto status = client_->Get(key_.Inode(ino), &value);
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
  auto status = client_->HGet(key_.Chunk(ino), std::to_string(idx), &value);
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
    auto status = client_->HScan(key_.Chunk(ino), cursor, kScanBatchSize, &values, &next_cursor);
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
  auto status = client_->Get(key_.InodeCount(), &value);
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
  return client_->Incr(key_.NextIno(), ino);
}

utils::Status RedisMetaOps::Transact(const std::function<utils::Status(Txn &)> &callback) {
  return client_->Transact([&](RedisKvTxn &kv_txn) {
    Txn txn(kv_txn, key_, chunk_size_);
    return callback(txn);
  });
}

}  // namespace swordfs::metadata
