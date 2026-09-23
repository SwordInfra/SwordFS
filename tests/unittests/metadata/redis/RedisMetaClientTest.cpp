// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/Conv.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "chunk/ChunkObjectKey.hpp"
#include "chunk/IChunkOverwriteStrategy.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Inode.hpp"
#include "utils/ExecutionDomain.hpp"

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

std::string UniqueRedisName(std::string_view suffix) {
  return swordfs::test::UniqueRedisTestNamespace("redis-meta-txn", suffix);
}

uint64_t RedisInfoCounter(sw::redis::Redis &redis, std::string_view section, std::string_view name) {
  const auto info = redis.info(section);
  const auto prefix = std::string(name) + ":";
  const auto begin = info.find(prefix) + prefix.size();
  const auto end = info.find('\r', begin);
  return folly::to<uint64_t>(std::string_view(info).substr(begin, end - begin));
}

utils::Status SeedInode(sw::redis::Redis &redis, const redis::RedisKey &key, const SwordFsInode &inode) {
  std::string value;
  auto status = inode.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  redis.set(key.Inode(inode.ino), value);
  return utils::Status::OK();
}

utils::Status SeedEntry(sw::redis::Redis &redis, const redis::RedisKey &key, InodeID parent_ino,
                        const SwordFsEntry &entry) {
  std::string value;
  auto status = entry.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  redis.hset(key.Directory(parent_ino), entry.name, value);
  return utils::Status::OK();
}

utils::Status SeedFrozenWholeObjectReclaim(sw::redis::Redis &redis, const redis::RedisKey &key, InodeID ino,
                                           const SwordFsChunk &chunk, ReclaimWork *work) {
  if (work == nullptr) {
    return utils::Status::InvalidArgument("reclaim work output is null");
  }

  SwordFsAttr attr(ino, S_IFREG | 0644);
  attr.nlink = 0;
  SwordFsInode inode(ino, attr, kRootInodeId);
  auto status = SeedInode(redis, key, inode);
  if (!status.ok()) {
    return status;
  }

  std::string chunk_value;
  status = chunk.SerializeTo(&chunk_value);
  if (!status.ok()) {
    return status;
  }
  redis.hset(key.Chunk(ino), std::to_string(chunk.index), chunk_value);
  redis.hset(key.Orphans(), std::to_string(ino), "1");
  redis.set(key.InodeCount(), "2");

  status = chunk::FreezeWholeObjectReclaim(ino, {chunk}, 4096, work);
  if (!status.ok()) {
    return status;
  }
  std::string encoded;
  status = work->SerializeTo(&encoded);
  if (!status.ok()) {
    return status;
  }
  redis.hset(key.Reclaims(), std::to_string(ino), encoded);
  return utils::Status::OK();
}

utils::Status CommitChunkTxn(RedisMetaClient &store, const redis::RedisKey &key, uint64_t chunk_size, InodeID ino,
                             const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement) {
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

  std::string_view name() const override {
    return "recording_redis";
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

}  // namespace

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

  EXPECT_EQ(status.code(), utils::Status::kIOError);
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
  EXPECT_TRUE(status.IsBusy());
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

  EXPECT_EQ(status.code(), utils::Status::kIOError);
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

TEST(RedisMetaClientTest, WatchConflictRetryLimitReturnsIOError) {
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

  EXPECT_EQ(status.code(), utils::Status::kIOError);
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

  EXPECT_EQ(status.code(), utils::Status::kIOError);
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
    EXPECT_EQ(txn.Get(key, nullptr).code(), utils::Status::kInvalidArgument);
    EXPECT_EQ(txn.HGet(key, "field", nullptr).code(), utils::Status::kInvalidArgument);
    EXPECT_EQ(txn.HLen(key, nullptr).code(), utils::Status::kInvalidArgument);
    uint64_t next_cursor = 0;
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_EQ(txn.HScan(key, 0, 16, nullptr, &next_cursor).code(), utils::Status::kInvalidArgument);
    EXPECT_EQ(txn.HScan(key, 0, 16, &values, nullptr).code(), utils::Status::kInvalidArgument);

    auto status = txn.Set(key, "value");
    if (!status.ok()) {
      return status;
    }
    std::string value;
    uint64_t length = 0;
    EXPECT_EQ(txn.Get(key, &value).code(), utils::Status::kInvalidArgument);
    EXPECT_EQ(txn.HGet(key, "field", &value).code(), utils::Status::kInvalidArgument);
    EXPECT_EQ(txn.HLen(key, &length).code(), utils::Status::kInvalidArgument);
    EXPECT_EQ(txn.HScan(key, 0, 16, &values, &next_cursor).code(), utils::Status::kInvalidArgument);
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

TEST(RedisMetaOpsTest, BackendContextMayBeReleasedByFiberAfterThreadShutdown) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  auto ops = std::make_unique<RedisMetaOps>(config, UniqueRedisName("lifecycle"));
  std::shared_ptr<DirIterator> iterator;
  RunInFiber([&] {
    ASSERT_TRUE(ops->CreateDirIterator(kRootInodeId, {}, &iterator).ok());
    ASSERT_NE(iterator, nullptr);
  });

  // RedisMetaOps is the thread-domain lifecycle owner. Its destructor drains
  // and clears the blocking backend resources while the iterator may still be
  // alive. Releasing the last iterator/context reference on a fiber must then
  // be safe because no blocking resource remains to be destroyed there.
  ops.reset();
  RunInFiber([&] { iterator.reset(); });
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

TEST(RedisMetaOpsTest, TransactionCallbackRunsInThreadDomain) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaOps ops(config, UniqueRedisName("callback-domain"));
  utils::ExecutionDomain callback_domain = utils::ExecutionDomain::kFiber;
  const auto status = RunInFiber([&] {
    return ops.TransactFromFiber([&](RedisMetaTxn &) {
      callback_domain = utils::CurrentExecutionDomain();
      return utils::Status::OK();
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(callback_domain, utils::ExecutionDomain::kThread);
}

TEST(RedisMetaOpsTest, GetInodeUsesDirectMetadataReadPath) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inode");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr attr(42, S_IFREG | 0644);
  attr.size = 1234;
  SwordFsInode inode(42, attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, inode).ok());

  SwordFsInode loaded;
  const auto status = RunInFiber([&] { return ops.GetInode(inode.ino, &loaded); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.ino, inode.ino);
  EXPECT_EQ(loaded.attr.size, inode.attr.size);

  const auto invalid_status = RunInFiber([&] { return ops.GetInode(inode.ino, nullptr); });
  EXPECT_EQ(invalid_status.code(), utils::Status::kInvalidArgument);
}

TEST(RedisMetaClientTest, MGetValidatesOutputAndEmptyInput) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient client(config);
  std::vector<std::optional<std::string>> values{std::string("stale")};
  ASSERT_TRUE(client.MGet({}, &values).ok());
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(client.MGet({"unused"}, nullptr).code(), utils::Status::kInvalidArgument);
}

TEST(RedisMetaClientTest, MGetReportsConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaClient client(config);
  std::vector<std::optional<std::string>> values;
  const auto status = client.MGet({"unreachable"}, &values);
  EXPECT_EQ(status.code(), utils::Status::kIOError) << status.message();
}

TEST(RedisMetaOpsTest, GetInodesUsesAlignedBatchReadWithMissingEntries) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inodes");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsInode first(42, SwordFsAttr(42, S_IFREG | 0644), kRootInodeId);
  first.attr.size = 1234;
  SwordFsInode second(43, SwordFsAttr(43, S_IFDIR | 0750), kRootInodeId);
  second.attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, first).ok());
  ASSERT_TRUE(SeedInode(redis, key, second).ok());

  std::vector<std::optional<SwordFsInode>> results;
  const auto status = RunInFiber([&] { return ops.GetInodes({first.ino, 999999, second.ino}, &results); });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(results.size(), 3u);
  ASSERT_TRUE(results[0].has_value());
  EXPECT_EQ(results[0]->attr.size, first.attr.size);
  EXPECT_FALSE(results[1].has_value());
  ASSERT_TRUE(results[2].has_value());
  EXPECT_EQ(results[2]->attr.nlink, second.attr.nlink);

  results.emplace_back(first);
  ASSERT_TRUE(RunInFiber([&] { return ops.GetInodes({}, &results); }).ok());
  EXPECT_TRUE(results.empty());

  const auto invalid_status = RunInFiber([&] { return ops.GetInodes({first.ino}, nullptr); });
  EXPECT_EQ(invalid_status.code(), utils::Status::kInvalidArgument);
}

TEST(RedisMetaOpsTest, GetInodesPropagatesConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaOps ops(config, "unreachable-get-inodes");
  std::vector<std::optional<SwordFsInode>> results;
  const auto status = RunInFiber([&] { return ops.GetInodes({42}, &results); });
  EXPECT_EQ(status.code(), utils::Status::kIOError) << status.message();
}

TEST(RedisMetaOpsTest, GetInodesRejectsMalformedAndMismatchedRecords) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inodes-invalid");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  constexpr InodeID kMalformedIno = 44;
  redis.set(key.Inode(kMalformedIno), "not-an-inode");
  std::vector<std::optional<SwordFsInode>> results;
  EXPECT_TRUE(RunInFiber([&] { return ops.GetInodes({kMalformedIno}, &results); }).IsMalformed());

  constexpr InodeID kRequestedIno = 46;
  SwordFsInode other(45, SwordFsAttr(45, S_IFREG | 0644), kRootInodeId);
  std::string encoded;
  ASSERT_TRUE(other.SerializeTo(&encoded).ok());
  redis.set(key.Inode(kRequestedIno), encoded);
  EXPECT_TRUE(RunInFiber([&] { return ops.GetInodes({kRequestedIno}, &results); }).IsMalformed());
}

TEST(RedisMetaOpsTest, LookupEntryOwnsReadOnlyTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-lookup-entry");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr child_attr(2, S_IFREG | 0644);
  SwordFsInode child(2, child_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, child).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"child", DT_REG, child.ino}).ok());

  SwordFsInode loaded;
  const auto status = RunInFiber([&] { return ops.LookupEntry(kRootInodeId, "child", &loaded); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.ino, child.ino);

  const auto invalid_status = RunInFiber([&] { return ops.LookupEntry(kRootInodeId, "child", nullptr); });
  EXPECT_EQ(invalid_status.code(), utils::Status::kInvalidArgument);
}

TEST(RedisMetaTxnTest, PrivateIndexPublicationCommitsAndRejectsWithLogicalHead) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-publication"));
  sw::redis::Redis redis(ConnectionOptions(config));
  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  RecordingRedisStrategy strategy;

  std::optional<PendingDelete> last_cleanup;
  auto publish = [&](const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement,
                     const ChunkPublishIntent &intent = {}) {
    utils::Status publication_result;
    std::optional<PendingDelete> cleanup_candidate;
    auto status = store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
      return txn.CommitChunk(file.ino, expected, replacement, publication_result, cleanup_candidate, intent);
    });
    last_cleanup = std::move(cleanup_candidate);
    return status.ok() ? publication_result : status;
  };

  const SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(publish(std::nullopt, first, ChunkPublishIntent{.payload = "manifest-one"}).ok());
  const auto private_hash = key.PrivateChunkIndex(strategy.name(), "fragments:" + std::to_string(file.ino));
  EXPECT_EQ(redis.hget(private_hash, "1"), std::optional<std::string>{"manifest-one"});
  std::string private_value;
  std::vector<std::pair<std::string, std::string>> fields;
  ASSERT_TRUE(store
                  .Transact([&](RedisKvTxn &kv_txn) {
                    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
                    auto status = txn.Read("fragments:" + std::to_string(file.ino), "1", &private_value);
                    if (!status.ok()) {
                      return status;
                    }
                    status = txn.Scan("fragments:" + std::to_string(file.ino), &fields);
                    if (!status.ok()) {
                      return status;
                    }
                    ChunkView view;
                    status = txn.LoadChunkView(file.ino, 0, &view);
                    if (!status.ok()) {
                      return status;
                    }
                    EXPECT_EQ(view.head, first);
                    EXPECT_EQ(view.private_snapshot, "manifest-one");
                    return utils::Status::OK();
                  })
                  .ok());
  EXPECT_EQ(private_value, "manifest-one");
  EXPECT_EQ(fields, (std::vector<std::pair<std::string, std::string>>{{"1", "manifest-one"}}));

  const SwordFsChunk replacement{.index = 0, .start_offset = 0, .revision = 2, .size = 96};
  strategy.index.reject_publish = true;
  EXPECT_EQ(publish(first, replacement, ChunkPublishIntent{.payload = "manifest-two"}).code(), utils::Status::kIOError);
  ASSERT_TRUE(last_cleanup.has_value());
  EXPECT_FALSE(redis.hget(private_hash, "2").has_value());
  SwordFsChunk head;
  const auto encoded = redis.hget(key.Chunk(file.ino), "0");
  ASSERT_TRUE(encoded.has_value());
  ASSERT_TRUE(head.ParseFrom(*encoded).ok());
  EXPECT_EQ(head, first);
}

TEST(RedisMetaTxnTest, EntryMutationsCarryStateThroughParameters) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("entries"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  redis.set(key.InodeCount(), "1");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);

    SwordFsAttr child_attr(2, S_IFDIR | 0755);
    SwordFsInode child(2, child_attr, kRootInodeId);
    return txn.AddEntry(kRootInodeId, "child", child, &root);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "child"));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "2");

  std::string value = redis.get(key.Inode(kRootInodeId)).value_or("");
  SwordFsInode persisted_root;
  ASSERT_TRUE(persisted_root.ParseFrom(value).ok());
  EXPECT_EQ(persisted_root.attr.nlink, 3U);
}

TEST(RedisMetaTxnTest, AddEntryRejectsExistingNameWithoutPersistingChild) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("add-entry-duplicate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr existing_attr(7, S_IFREG | 0644);
  SwordFsInode existing(7, existing_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, existing).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"child", DT_REG, existing.ino}).ok());
  redis.set(key.InodeCount(), "2");

  SwordFsAttr child_attr(8, S_IFREG | 0644);
  SwordFsInode child(8, child_attr, kRootInodeId);
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.AddEntry(kRootInodeId, "child", child, &root);
  });
  EXPECT_TRUE(status.IsAlreadyExists());
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "2");
}

TEST(RedisMetaTxnTest, MoveEntryPersistsExplicitState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("move-entry"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr child_attr(2, S_IFREG | 0644);
  SwordFsInode child(2, child_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, child).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"file", DT_REG, child.ino}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.MoveEntry(kRootInodeId, "file", kRootInodeId, "moved", &root, &root, &child, nullptr,
                         /*overwrite=*/true);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Directory(kRootInodeId), "file"));
  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "moved"));
}

TEST(RedisMetaTxnTest, RemoveDirectoryPersistsLifecycleState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("remove-directory"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr dir_attr(2, S_IFDIR | 0755);
  SwordFsInode dir(2, dir_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, dir).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"dir", DT_DIR, dir.ino}).ok());
  redis.set(key.InodeCount(), "2");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RemoveDirectory(kRootInodeId, "dir", &root, dir);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Directory(kRootInodeId), "dir"));
  EXPECT_FALSE(redis.exists(key.Inode(dir.ino)));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "1");

  std::string value = redis.get(key.Inode(kRootInodeId)).value_or("");
  SwordFsInode persisted_root;
  ASSERT_TRUE(persisted_root.ParseFrom(value).ok());
  EXPECT_EQ(persisted_root.attr.nlink, 2U);
}

TEST(RedisMetaTxnTest, ReadPrimitivesValidateOutputs) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("read-primitives"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(2, S_IFREG | 0644);
  SwordFsInode file(2, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    EXPECT_EQ(txn.LookupInode(file.ino, nullptr).code(), utils::Status::kInvalidArgument);

    SwordFsInode missing;
    auto status = txn.LookupEntry(file.ino, "missing", &missing);
    EXPECT_TRUE(status.IsNotDirectory());
    EXPECT_EQ(txn.LookupEntry(file.ino, "missing", nullptr).code(), utils::Status::kInvalidArgument);

    return utils::Status::OK();
  });
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST(RedisMetaTxnTest, LookupEntryRejectsDanglingDirectoryEntry) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("dangling-entry"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"dangling", DT_REG, 99}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    SwordFsInode out;
    return txn.LookupEntry(kRootInodeId, "dangling", &out);
  });
  EXPECT_TRUE(status.IsMalformed()) << status.message();
}

TEST(RedisMetaTxnTest, TruncateClampsPersistedBoundaryChunk) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-staged"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 8192;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk first_chunk{0, 0, 1, 4096};
  SwordFsChunk second_chunk{1, 4096, 2, 4096};
  std::string first_data;
  std::string second_data;
  ASSERT_TRUE(first_chunk.SerializeTo(&first_data).ok());
  ASSERT_TRUE(second_chunk.SerializeTo(&second_data).ok());
  redis.hset(key.Chunk(9), "0", first_data);
  redis.hset(key.Chunk(9), "1", second_data);

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(9, 1024, &detached);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(detached.size(), 1U);
  swordfs::chunk::WholeObjectRef detached_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(detached.front(), 4096, &detached_ref).ok());
  EXPECT_EQ(detached_ref.descriptor, second_chunk);

  const auto first_value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(first_value.has_value());
  SwordFsChunk first;
  ASSERT_TRUE(first.ParseFrom(*first_value).ok());
  EXPECT_EQ(first.size, 1024U);
  EXPECT_FALSE(redis.hexists(key.Chunk(9), "1"));

  file.attr.size = 4096;
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  const auto invalid_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 0);
    EXPECT_EQ(txn.Truncate(9, 1024).code(), utils::Status::kInternal);
    return utils::Status::OK();
  });
  EXPECT_TRUE(invalid_status.ok()) << invalid_status.message();
}

TEST(RedisMetaTxnTest, SetAttrShrinkWorksWithoutDetachedChunkOutput) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-no-cleanup-output"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 4096};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  SwordFsAttr requested = file.attr;
  requested.size = 0;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kSize);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Chunk(file.ino), "0"));

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.size, 0U);
}

TEST(RedisMetaTxnTest, RegisterPendingDeletesRejectsInvalidEnvelope) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("register-invalid-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const std::vector<PendingDelete> work{{.id = "", .index_format_version = 1, .payload = "opaque"}};

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RegisterPendingDeletes(work);
  });
  EXPECT_EQ(status.code(), utils::Status::kInvalidArgument);
  EXPECT_EQ(redis.hlen(key.PendingDeletes()), 0);
}

TEST(RedisMetaTxnTest, TruncateDoesNotDependOnPendingDeleteState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-independent-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 0, 1, 4096};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0, &detached);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(detached.size(), 1U);
  swordfs::chunk::WholeObjectRef detached_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(detached.front(), 4096, &detached_ref).ok());
  EXPECT_EQ(detached_ref.descriptor, chunk);
  EXPECT_FALSE(redis.hexists(key.Chunk(file.ino), "0"));
}

TEST(RedisMetaTxnTest, TruncateScansMultipleChunkHashPagesAndCollectsDetachedChunks) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-multipage-chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  constexpr uint64_t kChunkSize = 4096;
  constexpr uint32_t kChunkCount = 600;
  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = kChunkCount * kChunkSize;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  for (uint32_t index = 0; index < kChunkCount; ++index) {
    SwordFsChunk chunk{.index = index,
                       .start_offset = static_cast<uint64_t>(index) * kChunkSize,
                       .revision = static_cast<uint64_t>(index) + 1,
                       .size = kChunkSize};
    std::string encoded;
    ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
    redis.hset(key.Chunk(file.ino), std::to_string(index), encoded);
  }

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, kChunkSize);
    return txn.Truncate(file.ino, 0, &detached);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(detached.size(), kChunkCount);
  EXPECT_EQ(redis.hlen(key.Chunk(file.ino)), 0);
}

TEST(RedisMetaTxnTest, TruncateRejectsInvalidChunkIdentity) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-noncanonical-chunk"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk wrong{.index = 0, .start_offset = 1, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_TRUE(status.IsMalformed()) << status.message();
  EXPECT_TRUE(redis.hexists(key.Chunk(file.ino), "0"));

  redis.del(key.Chunk(file.ino));
  SwordFsChunk canonical{.index = 0, .start_offset = 0, .revision = 2, .size = 64};
  ASSERT_TRUE(canonical.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "1", encoded);
  const auto field_mismatch_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_TRUE(field_mismatch_status.IsMalformed()) << field_mismatch_status.message();
}

TEST(RedisMetaTxnTest, TruncatePropagatesWrongTypeChunkMapFromDestructivePhase) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-wrongtype-chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  redis.set(key.Chunk(file.ino), "wrong-type");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_FALSE(status.ok());

  SwordFsInode after;
  ASSERT_TRUE(after.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(after.attr.size, file.attr.size);
}

TEST(RedisMetaTxnTest, CommitChunkRejectsCorruptPersistedDescriptor) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-corrupt-current"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk corrupt{.index = 0, .start_offset = 1, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(corrupt.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  SwordFsChunk replacement{.index = 0, .start_offset = 0, .revision = 2, .size = 64};
  const auto status = CommitChunkTxn(store, key, 4096, file.ino, std::nullopt, replacement);
  EXPECT_TRUE(status.IsMalformed()) << status.message();
}

TEST(RedisMetaTxnTest, CommitChunkReturnsCleanupCandidateWithoutPendingDeleteDependency) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rewrite-cleanup-candidate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 64;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk expected{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(expected.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  auto replacement = expected;
  replacement.revision = 2;
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.CommitChunk(file.ino, expected, replacement, publication_result, cleanup_candidate);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(publication_result.ok()) << publication_result.message();
  ASSERT_TRUE(cleanup_candidate.has_value());
  swordfs::chunk::WholeObjectRef cleanup_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(*cleanup_candidate, 4096, &cleanup_ref).ok());
  EXPECT_EQ(cleanup_ref.descriptor, expected);
  const auto stored = redis.hget(key.Chunk(file.ino), "0");
  ASSERT_TRUE(stored.has_value());
  SwordFsChunk current;
  ASSERT_TRUE(current.ParseFrom(*stored).ok());
  EXPECT_EQ(current, replacement);
}

TEST(RedisMetaTxnTest, CommitChunkDefiniteRejectionReturnsReplacementAsCleanupCandidate) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("reject-cleanup-candidate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 64;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk current{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(current.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  auto replacement = current;
  replacement.revision = 2;
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.CommitChunk(file.ino, std::nullopt, replacement, publication_result, cleanup_candidate);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(publication_result.IsAlreadyExists()) << publication_result.message();
  ASSERT_TRUE(cleanup_candidate.has_value());
  swordfs::chunk::WholeObjectRef cleanup_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(*cleanup_candidate, 4096, &cleanup_ref).ok());
  EXPECT_EQ(cleanup_ref.descriptor, replacement);
}

TEST(RedisMetaTxnTest, CommitChunkRejectsConflictingInitialPublication) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-chunk"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 0, 1, 4096};
  const auto first_status = CommitChunkTxn(store, key, 4096, 9, std::nullopt, chunk);
  ASSERT_TRUE(first_status.ok()) << first_status.message();

  chunk.revision = 2;
  const auto duplicate_status = CommitChunkTxn(store, key, 4096, 9, std::nullopt, chunk);
  EXPECT_TRUE(duplicate_status.IsAlreadyExists()) << duplicate_status.message();

  const auto value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(value.has_value());
  SwordFsChunk persisted;
  ASSERT_TRUE(persisted.ParseFrom(*value).ok());
  EXPECT_EQ(persisted.revision, 1U);
}

TEST(RedisMetaTxnTest, FreezeReclaimScansAuthoritativeChunksInsideTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("reclaim-authoritative-scan"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.nlink = 0;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 0, 1, 4096};
  std::string chunk_data;
  ASSERT_TRUE(chunk.SerializeTo(&chunk_data).ok());
  redis.hset(key.Chunk(file.ino), "0", chunk_data);

  std::optional<ReclaimWork> work;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.FreezeReclaim(file.ino, work);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, 4096, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, chunk.index, chunk.revision));
  EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
  EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
  EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
}

TEST(RedisMetaTxnTest, FinalizeReclaimRejectsInvalidFrozenAuthorityBeforeMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  const SwordFsChunk chunk{0, 0, 1, 4096};
  RedisMetaClient store(config);
  sw::redis::Redis redis(ConnectionOptions(config));

  auto finalize = [&](const redis::RedisKey &key, const ReclaimWork &work) {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaTxn txn(kv_txn, key, 4096);
      return txn.FinalizeReclaim(kIno, work);
    });
  };

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-work-ino"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    work.ino = kIno + 1;
    const auto status = finalize(key, work);
    EXPECT_EQ(status.code(), utils::Status::kInvalidArgument);
    EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-malformed-reclaim"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    redis.hset(key.Reclaims(), std::to_string(kIno), "malformed");
    const auto status = finalize(key, work);
    EXPECT_TRUE(status.IsMalformed()) << status.message();
    EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-reclaim-ino-mismatch"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    ReclaimWork persisted = work;
    persisted.ino = kIno + 1;
    std::string encoded;
    ASSERT_TRUE(persisted.SerializeTo(&encoded).ok());
    redis.hset(key.Reclaims(), std::to_string(kIno), encoded);
    const auto status = finalize(key, work);
    EXPECT_TRUE(status.IsMalformed()) << status.message();
    EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-reclaim-content-mismatch"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    ReclaimWork different;
    const SwordFsChunk different_chunk{0, 0, 2, 4096};
    ASSERT_TRUE(chunk::FreezeWholeObjectReclaim(kIno, {different_chunk}, 4096, &different).ok());
    const auto status = finalize(key, different);
    EXPECT_TRUE(status.IsBusy()) << status.message();
    EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  }
}

TEST(RedisMetaTxnTest, FinalizeReclaimRejectsInvalidLiveStateBeforeMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  const SwordFsChunk chunk{0, 0, 1, 4096};
  RedisMetaClient store(config);
  sw::redis::Redis redis(ConnectionOptions(config));

  auto finalize = [&](const redis::RedisKey &key, const ReclaimWork &work) {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaTxn txn(kv_txn, key, 4096);
      return txn.FinalizeReclaim(kIno, work);
    });
  };
  auto expect_live = [&](const redis::RedisKey &key) {
    EXPECT_TRUE(redis.exists(key.Inode(kIno)));
    EXPECT_TRUE(redis.exists(key.Chunk(kIno)));
  };

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-missing-orphan"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    redis.hdel(key.Orphans(), std::to_string(kIno));
    const auto status = finalize(key, work);
    EXPECT_TRUE(status.IsMalformed()) << status.message();
    expect_live(key);
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-invalid-orphan"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    redis.hset(key.Orphans(), std::to_string(kIno), "invalid");
    const auto status = finalize(key, work);
    EXPECT_TRUE(status.IsMalformed()) << status.message();
    expect_live(key);
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-malformed-chunks"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    redis.del(key.Chunk(kIno));
    redis.set(key.Chunk(kIno), "wrong-type");
    const auto status = finalize(key, work);
    EXPECT_EQ(status.code(), utils::Status::kIOError);
    EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-changed-chunk"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    const SwordFsChunk changed{0, 0, 2, 4096};
    std::string encoded;
    ASSERT_TRUE(changed.SerializeTo(&encoded).ok());
    redis.hset(key.Chunk(kIno), "0", encoded);
    const auto status = finalize(key, work);
    EXPECT_TRUE(status.IsBusy()) << status.message();
    expect_live(key);
  }

  {
    const redis::RedisKey key(config.db, UniqueRedisName("finalize-missing-count"));
    ReclaimWork work;
    ASSERT_TRUE(SeedFrozenWholeObjectReclaim(redis, key, kIno, chunk, &work).ok());
    redis.del(key.InodeCount());
    const auto status = finalize(key, work);
    EXPECT_TRUE(status.IsNotFound()) << status.message();
    expect_live(key);
  }
}

}  // namespace swordfs::metadata
