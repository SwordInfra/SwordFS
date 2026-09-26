// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/Conv.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
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

#include "chunk/IChunkOverwriteStrategy.hpp"
#include "chunk/WholeObjectCleanup.hpp"
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
  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  if constexpr (std::is_void_v<Result>) {
    manager.addTask([&] {
      fn();
      done.post();
    });
    while (!done.try_wait()) {
      evb.loopOnce();
    }
    return;
  } else {
    std::optional<Result> result;
    manager.addTask([&] {
      result = fn();
      done.post();
    });
    while (!done.try_wait()) {
      evb.loopOnce();
    }
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

inline utils::Status CommitChunkTxn(RedisMetaClient &store, const redis::RedisKey &key, uint64_t chunk_size,
                                    InodeID ino, const std::optional<SwordFsChunk> &expected,
                                    const SwordFsChunk &replacement) {
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, chunk_size);
    return txn.CommitChunk(ino, expected, replacement, publication_result, cleanup_candidate);
  });
  if (!status.ok()) {
    return status;
  }
  return publication_result;
}

class RecordingRedisIndex final : public IChunkIndexParticipant {
 public:
  bool reject_publish = false;

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
};

class RecordingRedisStrategy final : public chunk::IChunkOverwriteStrategy {
 public:
  RecordingRedisIndex index;

  metadata::ChunkOverwriteMechanism mechanism() const override {
    return metadata::ChunkOverwriteMechanism::kRedisCache;
  }

  uint32_t index_format_version() const override {
    return 1;
  }

  std::shared_ptr<chunk::IChunkSession> OpenSession(InodeID file_ino, ChunkIndex chunk_index) const override {
    return chunk::DefaultChunkOverwriteStrategy().OpenSession(file_ino, chunk_index);
  }

  const IChunkIndexParticipant &index_participant() const override {
    return index;
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

  utils::Status DeletePending(const PendingDelete &work, uint64_t chunk_size, IMetaEngine *meta,
                              storage::IDataEngine *data, bool *completed) const override {
    return chunk::DefaultChunkOverwriteStrategy().DeletePending(work, chunk_size, meta, data, completed);
  }

  utils::Status DeleteFrozen(const ReclaimWork &work, uint64_t chunk_size, IMetaEngine *meta,
                             storage::IDataEngine *data) const override {
    return chunk::DefaultChunkOverwriteStrategy().DeleteFrozen(work, chunk_size, meta, data);
  }
};

}  // namespace swordfs::metadata
