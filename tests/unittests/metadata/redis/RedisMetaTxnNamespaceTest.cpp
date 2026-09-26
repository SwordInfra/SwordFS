// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"

namespace swordfs::metadata {

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
  EXPECT_TRUE(status.ToErrno() == EEXIST);
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "2");
}

TEST(RedisMetaTxnTest, AddEntryFailsClosedOnDanglingExistingEntry) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("add-entry-dangling"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"child", DT_REG, 99}).ok());

  SwordFsAttr child_attr(8, S_IFREG | 0644);
  SwordFsInode child(8, child_attr, root.ino);
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.AddEntry(root.ino, "child", child, &root);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "child"));
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
}

TEST(RedisMetaTxnTest, AddEntryPropagatesCorruptDirectoryBackendWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("add-entry-wrong-type"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  redis.set(key.Directory(root.ino), "not-a-directory-hash");

  SwordFsAttr child_attr(8, S_IFREG | 0644);
  SwordFsInode child(8, child_attr, root.ino);
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.AddEntry(root.ino, "child", child, &root);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(redis.get(key.Directory(root.ino)).value_or(""), "not-a-directory-hash");
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
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

TEST(RedisMetaTxnTest, RemoveDirectoryPropagatesCorruptChildDirectoryWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("remove-directory-wrong-type"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr dir_attr(2, S_IFDIR | 0755);
  SwordFsInode dir(2, dir_attr, root.ino);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, dir).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"dir", DT_DIR, dir.ino}).ok());
  redis.set(key.Directory(dir.ino), "not-a-directory-hash");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RemoveDirectory(root.ino, "dir", &root, dir);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "dir"));
  EXPECT_TRUE(redis.exists(key.Inode(dir.ino)));
  EXPECT_EQ(redis.get(key.Directory(dir.ino)).value_or(""), "not-a-directory-hash");
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
    EXPECT_EQ(txn.LookupInode(file.ino, nullptr).ToErrno(), EINVAL);

    SwordFsInode missing;
    auto status = txn.LookupEntry(file.ino, "missing", &missing);
    EXPECT_TRUE(status.ToErrno() == ENOTDIR);
    EXPECT_EQ(txn.LookupEntry(file.ino, "missing", nullptr).ToErrno(), EINVAL);

    return utils::Status::OK();
  });
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST(RedisMetaTxnTest, NamespaceMutationPrimitivesValidateStateContracts) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("namespace-validation"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr dir_attr(10, S_IFDIR | 0755);
  dir_attr.nlink = 2;
  SwordFsInode dir(10, dir_attr, kRootInodeId);
  SwordFsAttr other_dir_attr(11, S_IFDIR | 0755);
  other_dir_attr.nlink = 2;
  SwordFsInode other_dir(11, other_dir_attr, kRootInodeId);
  SwordFsAttr file_attr(20, S_IFREG | 0644);
  file_attr.nlink = 1;
  SwordFsInode file(20, file_attr, dir.ino);
  SwordFsAttr other_file_attr(21, S_IFREG | 0644);
  other_file_attr.nlink = 1;
  SwordFsInode other_file(21, other_file_attr, dir.ino);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);

    EXPECT_EQ(txn.LookupEntry(dir, "entry", nullptr).ToErrno(), EINVAL);

    EXPECT_EQ(txn.AddEntry(dir.ino, "entry", file, nullptr).ToErrno(), EINVAL);
    auto wrong_child_parent = file;
    wrong_child_parent.parent_ino = other_dir.ino;
    EXPECT_EQ(txn.AddEntry(dir.ino, "entry", wrong_child_parent, &dir).ToErrno(), EINVAL);
    auto wrong_parent = dir;
    wrong_parent.ino = other_dir.ino;
    EXPECT_EQ(txn.AddEntry(dir.ino, "entry", file, &wrong_parent).ToErrno(), EINVAL);

    EXPECT_EQ(txn.UnlinkFile(dir.ino, "entry", nullptr, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.UnlinkFile(dir.ino, "entry", &dir, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.UnlinkFile(other_dir.ino, "entry", &dir, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.UnlinkFile(dir.ino, "entry", &dir, &other_dir).ToErrno(), EINVAL);

    EXPECT_EQ(txn.RemoveDirectory(dir.ino, "entry", nullptr, other_dir).ToErrno(), EINVAL);
    EXPECT_EQ(txn.RemoveDirectory(other_dir.ino, "entry", &dir, other_dir).ToErrno(), EINVAL);
    EXPECT_EQ(txn.RemoveDirectory(dir.ino, "entry", &dir, file).ToErrno(), ENOTDIR);

    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", nullptr, &dir, &file, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, nullptr, &file, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &dir, nullptr, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(other_dir.ino, "old", dir.ino, "new", &dir, &dir, &file, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", other_dir.ino, "new", &dir, &dir, &file, nullptr, false).ToErrno(), EINVAL);
    auto same_id_parent = dir;
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &same_id_parent, &file, nullptr, false).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.MoveEntry(file.ino, "old", file.ino, "new", &file, &file, &other_file, nullptr, false).ToErrno(),
              ENOTDIR);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &other_file, false).ToErrno(), EEXIST);
    auto same_file = file;
    EXPECT_TRUE(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &same_file, true).ok());

    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", nullptr, &dir, &file, &other_file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, nullptr, &file, &other_file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &dir, nullptr, &other_file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(other_dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &other_file).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", other_dir.ino, "new", &dir, &dir, &file, &other_file).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &same_id_parent, &file, &other_file).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(file.ino, "old", file.ino, "new", &file, &file, &file, &other_file).ToErrno(),
              ENOTDIR);
    auto exchange_same_file = file;
    EXPECT_TRUE(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &exchange_same_file).ok());

    EXPECT_EQ(txn.LinkExistingEntry(dir.ino, "link", nullptr, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.LinkExistingEntry(dir.ino, "link", &dir, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.LinkExistingEntry(other_dir.ino, "link", &dir, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.LinkExistingEntry(file.ino, "link", &file, &other_file).ToErrno(), ENOTDIR);
    EXPECT_EQ(txn.LinkExistingEntry(dir.ino, "link", &dir, &other_dir).ToErrno(), EPERM);

    return utils::Status::OK();
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hlen(key.Directory(dir.ino)), 0);
  EXPECT_EQ(redis.hlen(key.Directory(other_dir.ino)), 0);
  EXPECT_FALSE(redis.exists(key.Inode(dir.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(other_dir.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(other_file.ino)));
}

TEST(RedisMetaTxnTest, AddEntryRejectsInvalidInodeIdentityWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("invalid-inode-identity"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr parent_attr(kRootInodeId, S_IFDIR | 0755);
  parent_attr.nlink = 2;
  SwordFsInode parent(kRootInodeId, parent_attr, kRootInodeId);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);

    SwordFsAttr zero_attr(0, S_IFREG | 0644);
    SwordFsInode zero_ino(0, zero_attr, parent.ino);
    EXPECT_EQ(txn.AddEntry(parent.ino, "zero", zero_ino, &parent).ToErrno(), EINVAL);

    SwordFsAttr mismatched_attr(31, S_IFREG | 0644);
    SwordFsInode mismatched(30, mismatched_attr, parent.ino);
    EXPECT_EQ(txn.AddEntry(parent.ino, "mismatch", mismatched, &parent).ToErrno(), EINVAL);
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hlen(key.Directory(parent.ino)), 0);
  EXPECT_FALSE(redis.exists(key.Inode(30)));
  EXPECT_FALSE(redis.exists(key.Inode(31)));
}

TEST(RedisMetaTxnTest, TouchInodePropagatesMissingInode) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("touch-missing-inode"));
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.TouchInode(424242, SetAttrField::kCtime);
  });

  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

TEST(RedisMetaTxnTest, MoveEntryRejectsCorruptDirectoryAncestryWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rename-parent-cycle"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr source_attr(2, S_IFDIR | 0755);
  source_attr.nlink = 2;
  SwordFsInode source(2, source_attr, kRootInodeId);
  SwordFsAttr first_cycle_attr(3, S_IFDIR | 0755);
  SwordFsInode first_cycle(3, first_cycle_attr, 4);
  SwordFsAttr second_cycle_attr(4, S_IFDIR | 0755);
  SwordFsInode second_cycle(4, second_cycle_attr, 3);

  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, source).ok());
  ASSERT_TRUE(SeedInode(redis, key, first_cycle).ok());
  ASSERT_TRUE(SeedInode(redis, key, second_cycle).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"source", DT_DIR, source.ino}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.MoveEntry(root.ino, "source", first_cycle.ino, "moved", &root, &first_cycle, &source, nullptr,
                         /*overwrite=*/false);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "source"));
  EXPECT_FALSE(redis.hexists(key.Directory(first_cycle.ino), "moved"));
  EXPECT_TRUE(redis.get(key.Inode(source.ino)).has_value());
}

TEST(RedisMetaTxnTest, MoveEntryRejectsMissingDirectoryAncestryWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rename-missing-parent"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr source_attr(2, S_IFDIR | 0755);
  source_attr.nlink = 2;
  SwordFsInode source(2, source_attr, root.ino);
  SwordFsAttr destination_attr(3, S_IFDIR | 0755);
  SwordFsInode destination(3, destination_attr, 999);

  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, source).ok());
  ASSERT_TRUE(SeedInode(redis, key, destination).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"source", DT_DIR, source.ino}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.MoveEntry(root.ino, "source", destination.ino, "moved", &root, &destination, &source, nullptr,
                         /*overwrite=*/false);
  });

  EXPECT_TRUE(status.IsNotFound()) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "source"));
  EXPECT_FALSE(redis.hexists(key.Directory(destination.ino), "moved"));
  EXPECT_TRUE(redis.get(key.Inode(source.ino)).has_value());
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
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
}
}  // namespace swordfs::metadata
