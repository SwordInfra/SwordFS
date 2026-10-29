// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/Conv.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
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

// Use the real Slice mechanism's encoder for shared Redis namespace and
// attribute contracts. These tests must not need a COW private fixture.
inline const chunk::internal::ChunkCleanupWorkEncoder &SliceEncoderForTest() {
  static const auto encoder = [] {
    std::unique_ptr<chunk::internal::ChunkCleanupWorkEncoder> result;
    auto status = chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kChunkSlice, &result);
    if (!status.ok()) {
      throw std::runtime_error(status.message());
    }
    return result;
  }();
  return *encoder;
}

}  // namespace swordfs::metadata
