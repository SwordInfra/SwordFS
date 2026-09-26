// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClientFaultServer.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

TEST(RedisMetaClientTest, BinaryValueRoundTrip) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("binary-roundtrip");
  const std::string value(172, '\0');
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, value);

  std::string actual;
  auto status = store.Get(key, &actual);
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
  const std::string parent_key = UniqueRedisName("txn-parent");
  const std::string child_key = UniqueRedisName("txn-child");
  const std::string child_value(172, '\0');
  std::atomic<bool> callback_in_thread_domain{false};

  auto status = store.Transact([&](RedisKvTxn &txn) {
    callback_in_thread_domain.store(utils::CurrentExecutionDomain() == utils::ExecutionDomain::kThread,
                                    std::memory_order_relaxed);
    std::string ignored;
    auto status = txn.Get(parent_key, &ignored);
    if (!status.IsNotFound()) {
      return status;
    }
    status = txn.Set(child_key, child_value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(parent_key, "parent");
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(callback_in_thread_domain.load(std::memory_order_relaxed));

  std::string actual;
  status = store.Get(child_key, &actual);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(actual.size(), child_value.size());
  EXPECT_EQ(actual, child_value);

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(parent_key);
  cleanup.del(child_key);
}

TEST(RedisMetaClientTest, DetectsExecCommandErrorAndReportsPossiblePartialCommit) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  sw::redis::Redis redis(ConnectionOptions(config));
  const std::string wrong_type_key = UniqueRedisName("exec-wrongtype");
  const std::string later_key = UniqueRedisName("exec-later-write");
  redis.del(wrong_type_key);
  redis.del(later_key);
  redis.set(wrong_type_key, "string-value");

  const auto status = store.Transact([&](RedisKvTxn &txn) {
    auto txn_status = txn.HSet(wrong_type_key, "field", "value");
    if (!txn_status.ok()) {
      return txn_status;
    }
    return txn.Set(later_key, "applied");
  });

  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_NE(status.message().find("commit may be partial"), std::string::npos);
  const auto later_value = redis.get(later_key);
  ASSERT_TRUE(later_value.has_value());
  EXPECT_EQ(*later_value, "applied");

  redis.del(wrong_type_key);
  redis.del(later_key);
}

TEST(RedisMetaClientTest, StandalonePingAndWatchReadMultiExec) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  auto status = store.Ping();
  ASSERT_TRUE(status.ok()) << status.message();

  const std::string key = UniqueRedisName("watch-read-write");
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, "before");

  status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    auto txn_status = transaction.Get(key, &value);
    if (!txn_status.ok()) {
      return txn_status;
    }
    EXPECT_EQ(value, "before");
    return transaction.Set(key, "ok");
  });
  ASSERT_TRUE(status.ok()) << status.message();

  status = store.Transact([&](RedisKvTxn &transaction) {
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
  EXPECT_TRUE(status.ok()) << status.message();
  cleanup.del(key);
}

TEST(RedisMetaClientTest, SuccessfulTransactionsReusePooledConnection) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  // A single-slot pool makes connection reuse observable without depending on
  // scheduler interleavings. The independent observer keeps one connection
  // open while reading Redis' cumulative accepted-connection counter.
  config.pool_size = 1;
  RedisMetaClient store(config);
  sw::redis::Redis observer(ConnectionOptions(config));
  const std::string key = UniqueRedisName("pooled-connection-reuse");
  observer.set(key, "0");

  auto transact_once = [&] {
    return store.Transact([&](RedisKvTxn &transaction) {
      std::string value;
      const auto status = transaction.Get(key, &value);
      EXPECT_TRUE(status.ok()) << status.message();
      return transaction.Set(key, value == "0" ? "1" : "0");
    });
  };

  // Warm the lazy redis++ pool before taking the baseline so normal initial
  // connection establishment is excluded from the measured delta.
  auto status = transact_once();
  ASSERT_TRUE(status.ok()) << status.message();
  const auto before = RedisInfoCounter(observer, "stats", "total_connections_received");

  constexpr int kTransactionCount = 64;
  for (int i = 0; i < kTransactionCount; ++i) {
    status = transact_once();
    ASSERT_TRUE(status.ok()) << "transaction " << i << ": " << status.message();
  }

  const auto after = RedisInfoCounter(observer, "stats", "total_connections_received");
  ASSERT_GE(after, before);
  const auto connection_delta = after - before;

  // The compose Redis healthcheck can contribute a small amount of background
  // traffic, so assert a generous bounded delta rather than an exact value.
  // A healthy one-slot pool reuses its connection; reconnect-per-transaction
  // behavior grows approximately with kTransactionCount and must fail here.
  EXPECT_LE(connection_delta, 16U) << "64 successful transactions opened " << connection_delta
                                   << " additional Redis connections";

  observer.del(key);
}

TEST(RedisMetaClientTest, RetriesWatchConflict) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string key = UniqueRedisName("watch-conflict");
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "before");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
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

  const std::string key = UniqueRedisName("read-only-watch-conflict");
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "before");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
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

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
  other.del(key);
}

TEST(RedisMetaClientTest, ReturnsCallbackBusyWithoutRetry) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &) {
    ++attempts;
    return utils::Status::Busy("filesystem operation is busy");
  });
  EXPECT_TRUE(status.ToErrno() == EBUSY);
  EXPECT_EQ(status.message(), "filesystem operation is busy");
  EXPECT_EQ(attempts, 1);
}

TEST(RedisMetaClientTest, DoesNotRetryWatchErrorEscapingFromCallback) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &) -> utils::Status {
    ++attempts;
    throw sw::redis::WatchError();
  });

  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(attempts, 1);
}

TEST(RedisMetaClientTest, RejectsNonPositiveRetryAttemptsDuringConstruction) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  for (const int retry_attempts : {0, -1}) {
    config.retry_attempts = retry_attempts;
    EXPECT_THROW({ RedisMetaClient store(config); }, std::invalid_argument);
  }
}

TEST(RedisMetaClientTest, WatchConflictRetryLimitReturnsUnavailable) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);

  const std::string key = UniqueRedisName("watch-retry-limit");
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "0");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    std::string value;
    auto status = transaction.Get(key, &value);
    if (!status.ok()) {
      return status;
    }
    other.incr(key);
    return transaction.Set(key, "committed");
  });

  EXPECT_TRUE(status.IsUnavailable());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(status.message(), "Redis transaction retry limit exceeded");
  EXPECT_EQ(attempts, 3);
  other.del(key);
}

TEST(RedisMetaClientTest, DoesNotRetryAmbiguousExecTimeout) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  sw::redis::Redis control(ConnectionOptions(config));
  const std::string key = UniqueRedisName("ambiguous-exec-timeout");
  control.del(key);

  config.socket_timeout = std::chrono::milliseconds(50);
  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);
  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    auto status = transaction.Set(key, "possibly-committed");
    if (!status.ok()) {
      return status;
    }

    // Pause after the write has been queued in Redis but before EXEC. The
    // client times out waiting for EXEC, whose commit result is therefore
    // ambiguous and must never be replayed.
    control.command<void>("CLIENT", "PAUSE", 1000, "ALL");
    return utils::Status::OK();
  });
  control.command<void>("CLIENT", "UNPAUSE");

  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_NE(status.message().find("ambiguous after EXEC"), std::string::npos);
  EXPECT_EQ(attempts, 1);
  control.del(key);
}

TEST(RedisMetaClientTest, DoesNotRetryAmbiguousExecConnectionClose) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  sw::redis::Redis control(ConnectionOptions(config));
  const std::string key = UniqueRedisName("ambiguous-exec-close");
  control.del(key);

  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);
  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    auto status = transaction.Set(key, "possibly-committed");
    if (!status.ok()) {
      return status;
    }

    // Keep this control connection alive while closing every other normal
    // client. The transaction's guarded connection is therefore closed before
    // exec() can obtain an acknowledgement; Commit must conservatively treat
    // that boundary as an unknown mutation outcome and must not replay it.
    const auto killed = control.command<long long>("CLIENT", "KILL", "TYPE", "normal", "SKIPME", "yes");
    EXPECT_GT(killed, 0);
    return utils::Status::OK();
  });

  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_NE(status.message().find("ambiguous after EXEC"), std::string::npos);
  EXPECT_EQ(attempts, 1);
  control.del(key);
}

TEST(RedisMetaClientTest, ReadOnlyTransactionCommitsAsNoOp) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("read-only");
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, "value");

  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    return transaction.Get(key, &value);
  });
  EXPECT_TRUE(status.ok()) << status.message();
  cleanup.del(key);
}

TEST(RedisMetaClientTest, KvTransactionValidatesOutputsAndRejectsReadAfterWrite) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("kv-validation");
  const auto status = store.Transact([&](RedisKvTxn &txn) {
    EXPECT_EQ(txn.Get(key, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HGet(key, "field", nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HLen(key, nullptr).ToErrno(), EINVAL);
    uint64_t next_cursor = 0;
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_EQ(txn.HScan(key, 0, 16, nullptr, &next_cursor).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HScan(key, 0, 16, &values, nullptr).ToErrno(), EINVAL);

    auto status = txn.Set(key, "value");
    if (!status.ok()) {
      return status;
    }
    std::string value;
    uint64_t length = 0;
    EXPECT_EQ(txn.Get(key, &value).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HGet(key, "field", &value).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HLen(key, &length).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HScan(key, 0, 16, &values, &next_cursor).ToErrno(), EINVAL);
    return utils::Status::OK();
  });
  EXPECT_TRUE(status.ok()) << status.message();

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(key);
}

TEST(RedisMetaClientTest, KvTransactionCommitsHashCounterAndDeleteMutations) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string hash_key = UniqueRedisName("kv-hash");
  const std::string counter_key = UniqueRedisName("kv-counter");
  const std::string deleted_key = UniqueRedisName("kv-delete");
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.del(hash_key);
  redis.set(counter_key, "10");
  redis.set(deleted_key, "value");
  redis.hset(hash_key, "old", "old-value");

  const auto status = store.Transact([&](RedisKvTxn &txn) {
    std::string value;
    auto status = txn.HGet(hash_key, "old", &value);
    if (!status.ok()) {
      return status;
    }
    EXPECT_EQ(value, "old-value");
    uint64_t length = 0;
    status = txn.HLen(hash_key, &length);
    if (!status.ok()) {
      return status;
    }
    EXPECT_EQ(length, 1U);

    status = txn.HSet(hash_key, "new", "new-value");
    if (!status.ok()) {
      return status;
    }
    status = txn.HDel(hash_key, "old");
    if (!status.ok()) {
      return status;
    }
    status = txn.IncrBy(counter_key, 5);
    if (!status.ok()) {
      return status;
    }
    return txn.Del(deleted_key);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hget(hash_key, "new").value_or(""), "new-value");
  EXPECT_FALSE(redis.hexists(hash_key, "old"));
  EXPECT_EQ(redis.get(counter_key).value_or(""), "15");
  EXPECT_FALSE(redis.exists(deleted_key));

  redis.del(hash_key);
  redis.del(counter_key);
}

TEST(RedisMetaClientTest, PreservesCallbackErrorWithoutQueuedWrite) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("callback-missing");
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    const auto get_status = transaction.Get(key, &value);
    if (!get_status.ok()) {
      return get_status;
    }
    return utils::Status::NotFound("expected missing key");
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

#ifndef NDEBUG
TEST(RedisMetaClientTest, RejectsFiberDomainCalls) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("wrong-domain");

  EXPECT_DEATH(
      {
        RunInFiber([&] {
          std::string value;
          (void)store.Get(key, &value);
        });
      },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}
#endif

TEST(RedisMetaClientTest, MGetValidatesOutputAndEmptyInput) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient client(config);
  std::vector<std::optional<std::string>> values{std::string("stale")};
  ASSERT_TRUE(client.MGet({}, &values).ok());
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(client.MGet({"unused"}, nullptr).ToErrno(), EINVAL);
}

TEST(RedisMetaClientTest, MGetReportsConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaClient client(config);
  std::vector<std::optional<std::string>> values;
  const auto status = client.MGet({"unreachable"}, &values);
  EXPECT_TRUE(status.IsUnavailable()) << status.message();
}
TEST(RedisMetaClientTest, RejectsConfigurationThatCanCreateUnboundedRedisWaits) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";

  auto invalid = config;
  invalid.connect_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.socket_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.pool_wait_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.pool_size = 0;
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.retry_backoff = std::chrono::milliseconds(-1);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);
}

TEST(RedisMetaClientTest, RetriesPreExecProtocolFailureAndReturnsUnavailable) {
  ScriptedRedisServer server(RedisFaultScenario::kPreExecProtocolFailure, 3);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    return transaction.Set("key", "value");
  });
  server.Wait();

  EXPECT_TRUE(status.IsUnavailable()) << status.message();
  EXPECT_EQ(attempts, 3);
  EXPECT_EQ(server.connection_count(), 3);
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, RetriesReadOnlyPreExecProtocolFailureAndReturnsUnavailable) {
  ScriptedRedisServer server(RedisFaultScenario::kReadOnlyPreExecProtocolFailure, 3);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  const auto status = store.Transact([](RedisKvTxn &) { return utils::Status::OK(); });
  server.Wait();

  EXPECT_TRUE(status.IsUnavailable()) << status.message();
  EXPECT_EQ(server.connection_count(), 3);
  EXPECT_FALSE(server.exec_received());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, ValidRedisReplyErrorsRemainKnownFailuresWithoutRetry) {
  {
    ScriptedRedisServer server(RedisFaultScenario::kReadOnlyReplyError);
    RedisMetaClient store(ScriptedConfig(server));
    const auto status = store.Ping();
    server.Wait();

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_FALSE(status.IsUnavailable());
    EXPECT_FALSE(status.IsOutcomeUnknown());
    EXPECT_EQ(server.connection_count(), 1);
    EXPECT_TRUE(server.error().empty()) << server.error();
  }

  {
    ScriptedRedisServer server(RedisFaultScenario::kWriteExecReplyError);
    auto config = ScriptedConfig(server);
    config.retry_attempts = 3;
    RedisMetaClient store(config);

    int attempts = 0;
    const auto status = store.Transact([&](RedisKvTxn &transaction) {
      ++attempts;
      return transaction.Set("key", "value");
    });
    server.Wait();

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_FALSE(status.IsUnavailable());
    EXPECT_FALSE(status.IsOutcomeUnknown());
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(server.exec_received());
    EXPECT_TRUE(server.error().empty()) << server.error();
  }
}

TEST(RedisMetaClientTest, PostExecProtocolFailureIsOutcomeUnknownWithoutReplay) {
  ScriptedRedisServer server(RedisFaultScenario::kWriteExecProtocolFailureApplied);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    return transaction.Set("key", "value");
  });
  server.Wait();

  EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
  EXPECT_EQ(attempts, 1);
  EXPECT_TRUE(server.exec_received());
  EXPECT_TRUE(server.mutation_applied());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, PostExecDisconnectCanBeNotAppliedAndStillOutcomeUnknown) {
  ScriptedRedisServer server(RedisFaultScenario::kWriteExecDisconnectNotApplied);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    return transaction.Set("key", "value");
  });
  server.Wait();

  EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
  EXPECT_EQ(attempts, 1);
  EXPECT_TRUE(server.exec_received());
  EXPECT_FALSE(server.mutation_applied());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, ReadOnlyExecProtocolFailureReturnsUnavailable) {
  ScriptedRedisServer server(RedisFaultScenario::kReadOnlyExecProtocolFailure);
  auto config = ScriptedConfig(server);

  RedisMetaClient store(config);
  const auto status = store.Transact([](RedisKvTxn &) { return utils::Status::OK(); });
  server.Wait();

  EXPECT_TRUE(status.IsUnavailable()) << status.message();
  EXPECT_TRUE(server.exec_received());
  EXPECT_FALSE(server.mutation_applied());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, DirectIncrReplyErrorIsKnownFailure) {
  ScriptedRedisServer server(RedisFaultScenario::kIncrReplyError);
  auto config = ScriptedConfig(server);
  RedisMetaClient store(config);

  uint64_t value = 0;
  const auto status = store.Incr("counter", &value);
  server.Wait();

  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_FALSE(status.IsUnavailable());
  EXPECT_FALSE(status.IsOutcomeUnknown());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, DirectIncrLostAcknowledgementIsOutcomeUnknownForBothDurableOutcomes) {
  for (const auto scenario :
       {RedisFaultScenario::kIncrProtocolFailureApplied, RedisFaultScenario::kIncrDisconnectNotApplied}) {
    ScriptedRedisServer server(scenario);
    auto config = ScriptedConfig(server);
    RedisMetaClient store(config);

    uint64_t value = 0;
    const auto status = store.Incr("counter", &value);
    server.Wait();

    EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
    EXPECT_TRUE(server.error().empty()) << server.error();
    if (scenario == RedisFaultScenario::kIncrProtocolFailureApplied) {
      EXPECT_TRUE(server.mutation_applied());
    } else {
      EXPECT_FALSE(server.mutation_applied());
    }
  }
}

TEST(RedisMetaClientTest, StandaloneReadOnlyConnectionFailuresReturnUnavailable) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.connect_timeout = std::chrono::milliseconds(50);
  config.socket_timeout = std::chrono::milliseconds(50);
  config.pool_wait_timeout = std::chrono::milliseconds(50);
  config.retry_attempts = 1;

  RedisMetaClient client(config);
  EXPECT_TRUE(client.Ping().IsUnavailable());

  std::string value;
  EXPECT_TRUE(client.Get("unreachable", &value).IsUnavailable());
  EXPECT_TRUE(client.HGet("unreachable", "field", &value).IsUnavailable());

  std::vector<std::optional<std::string>> values;
  EXPECT_TRUE(client.MGet({"unreachable"}, &values).IsUnavailable());

  std::vector<std::pair<std::string, std::string>> hash_values;
  uint64_t next_cursor = 0;
  EXPECT_TRUE(client.HScan("unreachable", 0, 16, &hash_values, &next_cursor).IsUnavailable());
}

}  // namespace swordfs::metadata
