// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Reclaim.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::test::redis_meta {

using metadata::InodeID;
using metadata::kRootInodeId;
using metadata::ReclaimWork;
using metadata::RedisMetaConfig;
using metadata::RedisMetaImpl;
using metadata::SetAttrField;
using metadata::SwordFsAttr;
using metadata::SwordFsChunk;
using metadata::SwordFsEntry;
using metadata::SwordFsInode;
using metadata::SwordFsVolume;
using utils::Status;
using utils::SwordFsContext;

inline constexpr uint64_t kTestChunkSize = 4096;

inline metadata::PendingDelete MakePendingDelete(InodeID ino, metadata::ChunkIndex index,
                                                 metadata::ChunkRevision revision) {
  SwordFsChunk descriptor{.index = index, .revision = revision, .size = 64};
  metadata::PendingDelete pending;
  const auto status = chunk::FreezeWholeObjectDelete(ino, descriptor, kTestChunkSize, &pending);
  EXPECT_TRUE(status.ok()) << status.message();
  return pending;
}

inline std::string PendingDeleteObjectKey(const metadata::PendingDelete &pending) {
  chunk::WholeObjectRef ref;
  const auto status = chunk::DecodeWholeObjectDelete(pending, kTestChunkSize, &ref);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok() ? ref.key : std::string{};
}

inline bool LoadConfig(RedisMetaConfig *config) {
  const char *url = std::getenv("SWORDFS_REDIS_TEST_URL");
  if (url == nullptr) {
    return false;
  }
  const auto status = metadata::ParseRedisMetaUrl(url, config);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok();
}

class RedisMetaImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!LoadConfig(&config_)) {
      GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
    }
    volume_name_ = swordfs::test::UniqueRedisTestNamespace("redis-meta-test");
    impl_ = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    auto status = impl_->Initialize();
    ASSERT_TRUE(status.ok()) << status.message();
    SwordFsVolume volume;
    volume.name = volume_name_;
    volume.chunk_size = 4096;
    status = impl_->FormatVolume(volume);
    ASSERT_TRUE(status.ok()) << status.message();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }

  template <typename Fn>
  decltype(auto) WithRawRedisOnThread(Fn &&fn) const {
    utils::ExpectInThreadDomain();
    sw::redis::ConnectionOptions options;
    options.host = config_.host;
    options.port = config_.port;
    options.db = config_.db;
    if (config_.username.has_value()) {
      options.user = *config_.username;
    }
    if (config_.password.has_value()) {
      options.password = *config_.password;
    }
    sw::redis::Redis redis(options);
    return std::invoke(std::forward<Fn>(fn), redis);
  }

  template <typename Fn>
  decltype(auto) RunWithRawRedisFromFiber(Fn &&fn) const {
    return swordfs::test::RunInTestThreadFromFiber(
        [this, fn = std::forward<Fn>(fn)]() mutable -> decltype(auto) { return WithRawRedisOnThread(std::move(fn)); });
  }

  std::vector<std::string> PendingDeleteKeys(RedisMetaImpl *impl = nullptr) const {
    std::vector<std::string> out;
    auto *target = impl != nullptr ? impl : impl_.get();
    bool has_more = false;
    do {
      auto status = target->VisitPendingDeletesBatch(
          128,
          [&out](const metadata::PendingDelete &work) {
            out.push_back(PendingDeleteObjectKey(work));
            return Status::OK();
          },
          &has_more);
      EXPECT_TRUE(status.ok()) << status.message();
      if (!status.ok()) {
        break;
      }
    } while (has_more);
    std::sort(out.begin(), out.end());
    return out;
  }

  void SeedPendingDelete(const metadata::PendingDelete &pending) const {
    const metadata::redis::RedisKey key(config_.db, volume_name_);
    std::string encoded;
    ASSERT_TRUE(pending.SerializeTo(&encoded).ok());
    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), pending.id, encoded); });
  }

  std::unique_ptr<RedisMetaImpl> impl_;
  RedisMetaConfig config_;
  std::string volume_name_;
};

}  // namespace swordfs::test::redis_meta
