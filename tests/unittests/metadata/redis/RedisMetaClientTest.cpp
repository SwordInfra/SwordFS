// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::metadata {
namespace {

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

const char *RedisTestUrl() {
  return std::getenv("SWORDFS_REDIS_TEST_URL");
}

bool ParseTestConfig(RedisMetaConfig *config) {
  const char *url = RedisTestUrl();
  if (url == nullptr) {
    return false;
  }
  const auto status = ParseRedisMetaUrl(url, config);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok();
}

sw::redis::ConnectionOptions ConnectionOptions(const RedisMetaConfig &config) {
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  options.db = config.db;
  return options;
}

}  // namespace

TEST(RedisMetaClientTest, BinaryValueRoundTrip) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = "swordfs:binary-roundtrip";
  const std::string value(172, '\0');
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, value);

  std::string actual;
  auto status = RunInFiber([&] { return store.Get(key, &actual); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(actual.size(), value.size());
  EXPECT_EQ(actual, value);
  cleanup.del(key);
}

TEST(RedisMetaClientTest, BinaryValueSurvivesWriteTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string parent_key = "swordfs:txn:parent";
  const std::string child_key = "swordfs:txn:child";
  const std::string child_value(172, '\0');

  auto status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &txn) {
      std::string ignored;
      auto s = txn.Get(parent_key, &ignored);
      if (!s.IsNotFound()) {
        return s;
      }
      s = txn.Set(child_key, child_value);
      if (!s.ok()) {
        return s;
      }
      return txn.Set(parent_key, "parent");
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();

  std::string actual;
  status = RunInFiber([&] { return store.Get(child_key, &actual); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(actual.size(), child_value.size());
  EXPECT_EQ(actual, child_value);

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(parent_key);
  cleanup.del(child_key);
}

TEST(RedisMetaClientTest, StandalonePingAndWatchReadMultiExec) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  auto status = RunInFiber([&] { return store.Ping(); });
  ASSERT_TRUE(status.ok()) << status.message();

  const std::string key = "swordfs:phase0:watch-read-write";
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, "before");

  status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &transaction) {
      std::string value;
      auto txn_status = transaction.Get(key, &value);
      if (!txn_status.ok()) {
        return txn_status;
      }
      EXPECT_EQ(value, "before");
      return transaction.Set(key, "ok");
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();

  status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &transaction) {
      std::string value;
      auto txn_status = transaction.Get(key, &value);
      if (!txn_status.ok()) {
        return txn_status;
      }
      if (value != "ok") {
        return utils::Status::IOError("unexpected Redis value");
      }
      return utils::Status::OK();
    });
  });
  EXPECT_TRUE(status.ok()) << status.message();
  cleanup.del(key);
}

TEST(RedisMetaClientTest, RetriesWatchConflict) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string key = "swordfs:phase0:watch-conflict";
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "before");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &transaction) {
      ++attempts;
      std::string value;
      auto txn_status = transaction.Get(key, &value);
      if (!txn_status.ok()) {
        return txn_status;
      }

      if (attempts == 1) {
        other.set(key, "raced");
      }
      return transaction.Set(key, "committed");
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
  const auto committed = other.get(key);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, "committed");
  other.del(key);
}

TEST(RedisMetaClientTest, RetriesReadOnlyWatchConflict) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string key = "swordfs:phase0:read-only-watch-conflict";
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "before");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &transaction) {
      ++attempts;
      std::string value;
      auto txn_status = transaction.Get(key, &value);
      if (!txn_status.ok()) {
        return txn_status;
      }

      if (attempts == 1) {
        other.set(key, "raced");
      }
      return utils::Status::OK();
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
  other.del(key);
}

TEST(RedisMetaClientTest, RetriesExplicitPreCommitFailure) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &transaction) {
      ++attempts;
      if (attempts == 1) {
        return utils::Status::Busy("retry");
      }
      return transaction.Set("swordfs:phase0:retry", "ok");
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
}

TEST(RedisMetaClientTest, RejectsNonPositiveRetryAttemptsDuringConstruction) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  for (const int retry_attempts : {0, -1}) {
    config.retry_attempts = retry_attempts;
    EXPECT_THROW({ RedisMetaClient store(config); }, std::invalid_argument);
  }
}

TEST(RedisMetaClientTest, RetriesUntilLimitIsExceeded) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 3;
  RedisMetaClient store(config);
  int attempts = 0;
  RunInFiber([&] {
    const auto status = store.Transact([&](RedisMetaTxn &) {
      ++attempts;
      return utils::Status::Busy("retry");
    });
    EXPECT_TRUE(status.IsBusy());
  });
  EXPECT_EQ(attempts, 3);
}

TEST(RedisMetaClientTest, ReadOnlyTransactionCommitsAsNoOp) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = "swordfs:phase0:read-only";
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, "value");

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisMetaTxn &transaction) {
      std::string value;
      return transaction.Get(key, &value);
    });
  });
  EXPECT_TRUE(status.ok()) << status.message();
  cleanup.del(key);
}

TEST(RedisMetaClientTest, PreservesCallbackErrorWithoutQueuedWrite) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const auto status = RunInFiber([&] {
    return store.Transact([](RedisMetaTxn &transaction) {
      std::string value;
      const auto get_status = transaction.Get("swordfs:phase0:missing", &value);
      if (!get_status.ok()) {
        return get_status;
      }
      return utils::Status::NotFound("expected missing key");
    });
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

}  // namespace swordfs::metadata
