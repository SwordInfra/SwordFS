// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
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

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Inode.hpp"

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

std::string UniqueVolumeName(std::string_view suffix) {
  static std::atomic<uint64_t> sequence{0};
  return "redis-meta-txn-" + std::string(suffix) + "-" + std::to_string(++sequence);
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
    return store.Transact([&](RedisKvTxn &txn) {
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
    return store.Transact([&](RedisKvTxn &transaction) {
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
    return store.Transact([&](RedisKvTxn &transaction) {
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
    return store.Transact([&](RedisKvTxn &transaction) {
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
    return store.Transact([&](RedisKvTxn &transaction) {
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
    return store.Transact([&](RedisKvTxn &transaction) {
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
    const auto status = store.Transact([&](RedisKvTxn &) {
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
    return store.Transact([&](RedisKvTxn &transaction) {
      std::string value;
      return transaction.Get(key, &value);
    });
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
  const std::string key = "swordfs:kv-validation";
  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &txn) {
      EXPECT_EQ(txn.Get(key, nullptr).code(), utils::Status::kInvalidArgument);
      EXPECT_EQ(txn.HGet(key, "field", nullptr).code(), utils::Status::kInvalidArgument);
      EXPECT_EQ(txn.HLen(key, nullptr).code(), utils::Status::kInvalidArgument);

      auto status = txn.Set(key, "value");
      if (!status.ok()) {
        return status;
      }
      std::string value;
      uint64_t length = 0;
      EXPECT_EQ(txn.Get(key, &value).code(), utils::Status::kInvalidArgument);
      EXPECT_EQ(txn.HGet(key, "field", &value).code(), utils::Status::kInvalidArgument);
      EXPECT_EQ(txn.HLen(key, &length).code(), utils::Status::kInvalidArgument);
      return utils::Status::OK();
    });
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
  const std::string hash_key = "swordfs:kv-hash";
  const std::string counter_key = "swordfs:kv-counter";
  const std::string deleted_key = "swordfs:kv-delete";
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.del(hash_key);
  redis.set(counter_key, "10");
  redis.set(deleted_key, "value");
  redis.hset(hash_key, "old", "old-value");

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &txn) {
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
  const auto status = RunInFiber([&] {
    return store.Transact([](RedisKvTxn &transaction) {
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

TEST(RedisMetaOpsTest, GetInodeUsesDirectMetadataReadPath) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueVolumeName("ops-get-inode");
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

TEST(RedisMetaOpsTxnTest, EntryMutationsCarryStateThroughParameters) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("entries"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  redis.set(key.InodeCount(), "1");

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);

      bool empty = false;
      auto status = txn.IsDirEmpty(kRootInodeId, &empty);
      if (!status.ok()) {
        return status;
      }
      EXPECT_TRUE(empty);

      bool exists = true;
      status = txn.EntryExists(kRootInodeId, "child", &exists);
      if (!status.ok()) {
        return status;
      }
      EXPECT_FALSE(exists);

      SwordFsAttr child_attr(2, S_IFDIR | 0755);
      SwordFsInode child(2, child_attr, kRootInodeId);
      status = txn.InsertInode(child);
      if (!status.ok()) {
        return status;
      }
      status = txn.LinkEntry(kRootInodeId, "child", child, &root);
      if (!status.ok()) {
        return status;
      }
      status = txn.SetInode(root);
      if (!status.ok()) {
        return status;
      }
      return txn.AdjustInodeCount(1);
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "child"));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "2");

  std::string value = redis.get(key.Inode(kRootInodeId)).value_or("");
  SwordFsInode persisted_root;
  ASSERT_TRUE(persisted_root.ParseFrom(value).ok());
  EXPECT_EQ(persisted_root.attr.nlink, 3U);
}

TEST(RedisMetaOpsTxnTest, InsertInodeRejectsPersistedInode) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("insert"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr original_attr(7, S_IFREG | 0644);
  SwordFsInode original(7, original_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, original).ok());

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);
      return txn.InsertInode(original);
    });
  });
  EXPECT_TRUE(status.IsAlreadyExists());
}

TEST(RedisMetaOpsTxnTest, EntryRemovalUsesExplicitParentAndTargetState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("dir-empty"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr child_attr(2, S_IFREG | 0644);
  SwordFsInode child(2, child_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, child).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"file", DT_REG, child.ino}).ok());

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);
      bool empty = true;
      auto status = txn.IsDirEmpty(kRootInodeId, &empty);
      if (!status.ok()) {
        return status;
      }
      EXPECT_FALSE(empty);

      status = txn.UnlinkEntry(kRootInodeId, "file", child, &root);
      if (!status.ok()) {
        return status;
      }
      return txn.SetInode(root);
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hlen(key.Directory(kRootInodeId)), 0);
}

TEST(RedisMetaOpsTxnTest, DeletePrimitivesPersistWithoutWorkingSet) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("deletes"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr dir_attr(2, S_IFDIR | 0755);
  SwordFsInode dir(2, dir_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, dir).ok());
  ASSERT_TRUE(SeedEntry(redis, key, dir.ino, SwordFsEntry{"stale", DT_REG, 3}).ok());

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);
      auto status = txn.DeleteDirectory(dir.ino);
      if (!status.ok()) {
        return status;
      }
      return txn.DeleteInode(dir.ino);
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.exists(key.Directory(dir.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(dir.ino)));
}

TEST(RedisMetaOpsTxnTest, ReadPrimitivesValidateOutputsAndDetectParentCycles) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("read-primitives"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(2, S_IFREG | 0644);
  SwordFsInode file(2, file_attr, kRootInodeId);
  SwordFsAttr first_attr(3, S_IFDIR | 0755);
  SwordFsInode first(3, first_attr, 4);
  SwordFsAttr second_attr(4, S_IFDIR | 0755);
  SwordFsInode second(4, second_attr, 3);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  ASSERT_TRUE(SeedInode(redis, key, first).ok());
  ASSERT_TRUE(SeedInode(redis, key, second).ok());

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);
      EXPECT_EQ(txn.LookupInode(file.ino, nullptr).code(), utils::Status::kInvalidArgument);

      bool exists = true;
      auto status = txn.EntryExists(file.ino, "missing", &exists);
      if (!status.ok()) {
        return status;
      }
      EXPECT_FALSE(exists);
      EXPECT_EQ(txn.EntryExists(file.ino, "missing", nullptr).code(), utils::Status::kInvalidArgument);

      bool empty = false;
      EXPECT_TRUE(txn.IsDirEmpty(file.ino, &empty).IsNotDirectory());
      EXPECT_EQ(txn.IsDirEmpty(file.ino, nullptr).code(), utils::Status::kInvalidArgument);

      bool descendant = false;
      EXPECT_TRUE(txn.IsDescendantOf(99, first.ino, &descendant).IsMalformed());
      EXPECT_EQ(txn.IsDescendantOf(99, first.ino, nullptr).code(), utils::Status::kInvalidArgument);
      return utils::Status::OK();
    });
  });
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST(RedisMetaOpsTxnTest, TruncateChunksClampsPersistedBoundaryChunk) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("truncate-staged"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsChunk first_chunk{0, 0, "first", 4096};
  SwordFsChunk second_chunk{1, 4096, "second", 4096};
  std::string first_data;
  std::string second_data;
  ASSERT_TRUE(first_chunk.SerializeTo(&first_data).ok());
  ASSERT_TRUE(second_chunk.SerializeTo(&second_data).ok());
  redis.hset(key.Chunk(9), "0", first_data);
  redis.hset(key.Chunk(9), "1", second_data);

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);
      return txn.TruncateChunks(9, 8192, 1024);
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();

  const auto first_value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(first_value.has_value());
  SwordFsChunk first;
  ASSERT_TRUE(first.ParseFrom(*first_value).ok());
  EXPECT_EQ(first.size, 1024U);
  EXPECT_FALSE(redis.hexists(key.Chunk(9), "1"));

  const auto invalid_status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 0);
      EXPECT_EQ(txn.TruncateChunks(9, 4096, 1024).code(), utils::Status::kInternal);
      return utils::Status::OK();
    });
  });
  EXPECT_TRUE(invalid_status.ok()) << invalid_status.message();
}

TEST(RedisMetaOpsTxnTest, SetChunkAfterDeleteChunksKeepsNewChunk) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueVolumeName("chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsChunk old_chunk{0, 0, "old", 4096};
  std::string value;
  ASSERT_TRUE(old_chunk.SerializeTo(&value).ok());
  redis.hset(key.Chunk(9), "0", value);

  const auto status = RunInFiber([&] {
    return store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaOps::Txn txn(kv_txn, key, 4096);
      auto status = txn.DeleteChunks(9);
      if (!status.ok()) {
        return status;
      }
      SwordFsChunk new_chunk{1, 4096, "new", 1024};
      return txn.SetChunk(9, new_chunk);
    });
  });
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_FALSE(redis.hexists(key.Chunk(9), "0"));
  const auto new_value = redis.hget(key.Chunk(9), "1");
  ASSERT_TRUE(new_value.has_value());
  SwordFsChunk persisted;
  ASSERT_TRUE(persisted.ParseFrom(*new_value).ok());
  EXPECT_EQ(persisted.key, "new");
}

}  // namespace swordfs::metadata
