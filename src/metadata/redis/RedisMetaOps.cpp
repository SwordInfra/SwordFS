// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaOps.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "config/ConfigCenter.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisDirIterator.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

RedisMetaOps::RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name)
    : backend_(std::make_shared<RedisBackendContext>(
          config, static_cast<size_t>(std::max(1, swordfs::config::ConfigCenter::Instance().meta_thread_count())))),
      key_(config.db, volume_name) {
  utils::ExpectInThreadDomain();
}

RedisMetaOps::~RedisMetaOps() {
  utils::ExpectInThreadDomain();
  backend_->Shutdown();
}

utils::Status RedisMetaOps::Initialize() {
  utils::ExpectInThreadDomain();
  return backend_->executor().RunFromThread([this] { return backend_->client().Ping(); });
}

utils::Status RedisMetaOps::FormatVolume(const SwordFsVolume &config) {
  utils::ExpectInThreadDomain();

  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0755);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  status = backend_->executor().RunFromThread([&] {
    return backend_->client().Transact([&](RedisKvTxn &txn) {
      std::string existing;
      auto txn_status = txn.Get(key_.Format(), &existing);
      if (txn_status.ok()) {
        return utils::Status::AlreadyExists("Redis metadata volume is already formatted");
      }
      if (!txn_status.IsNotFound()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.Format(), config.SerializeTo());
      if (!txn_status.ok()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.NextIno(), std::to_string(kRootInodeId));
      if (!txn_status.ok()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.InodeCount(), "1");
      if (!txn_status.ok()) {
        return txn_status;
      }
      return txn.Set(key_.Inode(kRootInodeId), root_value);
    });
  });
  if (status.ok()) {
    chunk_size_ = config.chunk_size;
  }
  return status;
}

utils::Status RedisMetaOps::LoadVolume(SwordFsVolume *config) {
  utils::ExpectInThreadDomain();
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis volume config output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromThread([&] { return backend_->client().Get(key_.Format(), &value); });
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
  utils::ExpectInFiberDomain();
  if (iterator == nullptr) {
    return utils::Status::InvalidArgument("directory iterator output is null");
  }
  *iterator = std::make_shared<RedisDirIterator>(backend_, key_.Directory(ino), std::move(prefix_entries));
  return utils::Status::OK();
}

utils::Status RedisMetaOps::GetInode(InodeID ino, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromFiber([&] { return backend_->client().Get(key_.Inode(ino), &value); });
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

utils::Status RedisMetaOps::LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Lookup output is null");
  }

  SwordFsInode result;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) { return txn.LookupEntry(parent_ino, name, &result); });
  if (status.ok()) {
    *out = result;
  }
  return status;
}

utils::Status RedisMetaOps::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  SwordFsInode result;
  auto status = TransactFromFiber(
      [&](RedisMetaTxn &txn) { return txn.SetAttr(ino, requested, fields, out != nullptr ? &result : nullptr); });
  if (status.ok() && out != nullptr) {
    *out = result;
  }
  return status;
}

utils::Status RedisMetaOps::Truncate(InodeID ino, uint64_t size) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.Truncate(ino, size); });
}

utils::Status RedisMetaOps::TouchInode(InodeID ino, SetAttrField fields) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.TouchInode(ino, fields); });
}

utils::Status RedisMetaOps::ReclaimInode(InodeID ino) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.ReclaimInode(ino); });
}

utils::Status RedisMetaOps::AddChunk(InodeID ino, const SwordFsChunk &chunk) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.AddChunk(ino, chunk); });
}

utils::Status RedisMetaOps::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  utils::ExpectInFiberDomain();
  if (chunk == nullptr) {
    return utils::Status::InvalidArgument("chunk output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromFiber(
      [&] { return backend_->client().HGet(key_.Chunk(ino), std::to_string(idx), &value); });
  if (!status.ok()) {
    return status;
  }
  return chunk->ParseFrom(value);
}

utils::Status RedisMetaOps::VisitChunks(InodeID ino,
                                        const std::function<utils::Status(const SwordFsChunk &)> &visitor) {
  utils::ExpectInFiberDomain();
  if (!visitor) {
    return utils::Status::InvalidArgument("chunk visitor is null");
  }

  constexpr size_t kScanBatchSize = 128;
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    auto status = backend_->executor().RunFromFiber(
        [&] { return backend_->client().HScan(key_.Chunk(ino), cursor, kScanBatchSize, &values, &next_cursor); });
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
  utils::ExpectInFiberDomain();
  if (count == nullptr) {
    return utils::Status::InvalidArgument("inode count output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromFiber([&] { return backend_->client().Get(key_.InodeCount(), &value); });
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
  utils::ExpectInFiberDomain();
  if (ino == nullptr) {
    return utils::Status::InvalidArgument("inode id output is null");
  }
  return backend_->executor().RunFromFiber([&] { return backend_->client().Incr(key_.NextIno(), ino); });
}

utils::Status RedisMetaOps::TransactFromFiber(const std::function<utils::Status(RedisMetaTxn &)> &callback) {
  utils::ExpectInFiberDomain();
  return backend_->executor().RunFromFiber([&] {
    return backend_->client().Transact([&](RedisKvTxn &kv_txn) {
      utils::ExpectInThreadDomain();
      RedisMetaTxn txn(kv_txn, key_, chunk_size_);
      return callback(txn);
    });
  });
}

}  // namespace swordfs::metadata
