// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/Conv.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "chunk/internal/ChunkMetadataBridge.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Reclaim.hpp"

namespace swordfs::metadata {

inline constexpr SetAttrField kKillSuidGidField = SetAttrField::kKillSuidGid;

template <typename Fn>
auto RunInFiber(Fn &&fn) -> decltype(fn()) {
  using Result = decltype(fn());
  if constexpr (std::is_void_v<Result>) {
    swordfs::test::RunInTestFiber(std::forward<Fn>(fn));
    return;
  } else {
    std::optional<Result> result;
    swordfs::test::RunInTestFiber([&] { result.emplace(fn()); });
    return std::move(*result);
  }
}

inline const char *RedisTestUrl() {
  return std::getenv("SWORDFS_REDIS_TEST_URL");
}

inline bool ParseTestConfig(RedisMetaConfig *config) {
  const char *url = RedisTestUrl();
  if (url == nullptr) {
    return false;
  }
  const auto status = ParseRedisMetaUrl(url, config);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok();
}

inline sw::redis::ConnectionOptions ConnectionOptions(const RedisMetaConfig &config) {
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  options.db = config.db;
  return options;
}

inline std::string UniqueRedisName(std::string_view suffix) {
  return swordfs::test::UniqueRedisTestNamespace("redis-meta-txn", suffix);
}

inline uint64_t RedisInfoCounter(sw::redis::Redis &redis, std::string_view section, std::string_view name) {
  const auto info = redis.info(section);
  const auto prefix = std::string(name) + ":";
  const auto begin = info.find(prefix) + prefix.size();
  const auto end = info.find('\r', begin);
  return folly::to<uint64_t>(std::string_view(info).substr(begin, end - begin));
}

inline utils::Status SeedInode(sw::redis::Redis &redis, const redis::RedisKey &key, const SwordFsInode &inode) {
  std::string value;
  auto status = inode.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  redis.set(key.Inode(inode.ino), value);
  return utils::Status::OK();
}

inline utils::Status SeedEntry(sw::redis::Redis &redis, const redis::RedisKey &key, InodeID parent_ino,
                               const SwordFsEntry &entry) {
  std::string value;
  auto status = entry.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  redis.hset(key.Directory(parent_ino), entry.name, value);
  return utils::Status::OK();
}

inline const chunk::internal::ChunkMetadataBridge &WholeObjectBridgeForTest();

inline utils::Status CommitChunkTxn(RedisMetaClient &store, const redis::RedisKey &key, uint64_t chunk_size,
                                    InodeID ino, const std::optional<SwordFsChunk> &expected,
                                    const SwordFsChunk &replacement) {
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, chunk_size, ChunkOverwriteMechanism::kWholeObject, &WholeObjectBridgeForTest());
    return txn.CommitChunk(ino, expected, replacement, publication_result, cleanup_candidate);
  });
  if (!status.ok()) {
    return status;
  }
  return publication_result;
}

class WholeObjectTestBridge final : public chunk::internal::ChunkMetadataBridge {
 public:
  utils::Status LoadPublished(IChunkIndexReader &, InodeID, const SwordFsChunk &,
                              std::string *private_snapshot) const override {
    if (private_snapshot == nullptr) {
      return utils::Status::InvalidArgument("whole-object private snapshot output is null");
    }
    private_snapshot->clear();
    return utils::Status::OK();
  }

  utils::Status Publish(IChunkIndexTxn &, InodeID, const std::optional<SwordFsChunk> &, const SwordFsChunk &,
                        const ChunkPublishIntent &intent) const override {
    if (!intent.payload.empty()) {
      return utils::Status::InvalidArgument("whole-object publication intent must be empty");
    }
    return utils::Status::OK();
  }

  utils::Status Truncate(IChunkIndexTxn &, InodeID, const std::vector<ChunkIndexChange> &) const override {
    return utils::Status::OK();
  }

  utils::Status PrepareReclaim(IChunkIndexTxn &, InodeID, const std::vector<SwordFsChunk> &) const override {
    return utils::Status::OK();
  }

  utils::Status FreezePendingDelete(IChunkIndexTxn &, InodeID file_ino, const SwordFsChunk &head, uint64_t chunk_size,
                                    PendingDelete *out) const override {
    return chunk::FreezeWholeObjectDelete(file_ino, head, chunk_size, out);
  }

  utils::Status FreezeRejectedPublication(InodeID file_ino, const SwordFsChunk &replacement,
                                          const ChunkPublishIntent &intent, uint64_t chunk_size,
                                          PendingDelete *out) const override {
    if (!intent.payload.empty()) {
      return utils::Status::InvalidArgument("whole-object publication intent must be empty");
    }
    return chunk::FreezeWholeObjectDelete(file_ino, replacement, chunk_size, out);
  }

  utils::Status FreezeReclaim(IChunkIndexTxn &, InodeID file_ino, const std::vector<SwordFsChunk> &heads,
                              uint64_t chunk_size, ReclaimWork *out) const override {
    return chunk::FreezeWholeObjectReclaim(file_ino, heads, chunk_size, out);
  }
};

inline const chunk::internal::ChunkMetadataBridge &WholeObjectBridgeForTest() {
  static const WholeObjectTestBridge bridge;
  return bridge;
}

class RecordingRedisBridge final : public chunk::internal::ChunkMetadataBridge {
 public:
  bool reject_publish = false;

  ChunkOverwriteMechanism mechanism() const {
    return ChunkOverwriteMechanism::kRedisCache;
  }

  utils::Status LoadPublished(IChunkIndexReader &reader, InodeID file_ino, const SwordFsChunk &head,
                              std::string *private_snapshot) const override {
    return reader.Read("fragments:" + std::to_string(file_ino), std::to_string(head.revision), private_snapshot);
  }

  utils::Status Publish(IChunkIndexTxn &txn, InodeID file_ino, const std::optional<SwordFsChunk> &,
                        const SwordFsChunk &replacement, const ChunkPublishIntent &intent) const override {
    // This test record is auxiliary; a real slice manifest needed for reads
    // must be durable before the public head is queued in Redis EXEC.
    const auto value = intent.payload.empty() ? "staged" : intent.payload;
    auto status = txn.Put("fragments:" + std::to_string(file_ino), std::to_string(replacement.revision), value);
    if (!status.ok()) {
      return status;
    }
    return reject_publish ? utils::Status::IOError("reject private publication") : utils::Status::OK();
  }

  utils::Status Truncate(IChunkIndexTxn &, InodeID, const std::vector<ChunkIndexChange> &) const override {
    return utils::Status::OK();
  }

  utils::Status PrepareReclaim(IChunkIndexTxn &, InodeID, const std::vector<SwordFsChunk> &) const override {
    return utils::Status::OK();
  }

  utils::Status FreezePendingDelete(IChunkIndexTxn &, InodeID file_ino, const SwordFsChunk &head, uint64_t chunk_size,
                                    PendingDelete *out) const override {
    return chunk::FreezeWholeObjectDelete(file_ino, head, chunk_size, out);
  }

  utils::Status FreezeRejectedPublication(InodeID file_ino, const SwordFsChunk &replacement, const ChunkPublishIntent &,
                                          uint64_t chunk_size, PendingDelete *out) const override {
    return chunk::FreezeWholeObjectDelete(file_ino, replacement, chunk_size, out);
  }

  utils::Status FreezeReclaim(IChunkIndexTxn &, InodeID file_ino, const std::vector<SwordFsChunk> &heads,
                              uint64_t chunk_size, ReclaimWork *out) const override {
    return chunk::FreezeWholeObjectReclaim(file_ino, heads, chunk_size, out);
  }
};

}  // namespace swordfs::metadata
