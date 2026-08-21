// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaStore.hpp"

namespace swordfs::metadata {
namespace {

const char* RedisTestUrl() {
  return std::getenv("SWORDFS_REDIS_TEST_URL");
}

}  // namespace

TEST(RedisMetaStoreTest, StandalonePingAndTransaction) {
  const char* url = RedisTestUrl();
  if (url == nullptr) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl(url, &config);
  ASSERT_TRUE(status.ok()) << status.message();

  RedisMetaStore store(config);
  status = store.Ping();
  ASSERT_TRUE(status.ok()) << status.message();

  const std::string key = "swordfs:phase0:test";
  status = store.Transact([&](sw::redis::Transaction& transaction) {
    transaction.set(key, "ok");
    return utils::Status::OK();
  });
  ASSERT_TRUE(status.ok()) << status.message();
}

TEST(RedisMetaStoreTest, RetriesExplicitPreCommitFailure) {
  const char* url = RedisTestUrl();
  if (url == nullptr) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl(url, &config);
  ASSERT_TRUE(status.ok()) << status.message();

  RedisMetaStore store(config);
  int attempts = 0;
  status = store.Transact([&](sw::redis::Transaction& transaction) {
    ++attempts;
    if (attempts == 1) {
      return utils::Status::Busy("retry");
    }
    transaction.set("swordfs:phase0:retry", "ok");
    return utils::Status::OK();
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
}

}  // namespace swordfs::metadata
