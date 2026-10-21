// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/ScopeGuard.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <cerrno>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkTestSupport.hpp"
#include "chunk/slice/SliceChunk.hpp"
#include "chunk/slice/SliceRecord.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaClientFaultServer.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisSliceChunkMetadata.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "utils/Context.hpp"

namespace swordfs::metadata {
namespace {

using chunk::slice::ResolvedSlice;
using chunk::slice::SliceID;
using chunk::slice::SliceRecord;

// #271 integration: exercise production SliceChunk::Flush through two actual
// RedisMetaImpl instances, not a hand-written FileMetadata mock. The object
// engine is shared in-memory solely to isolate Redis metadata publication
// from #275's later FUSE/S3 integration tests.
TEST(RedisSliceChunkMetadataTest, SliceChunkPublisherRemountReadsAndOverwritesAcrossRedisClients) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  constexpr uint64_t kChunkSize = 4096;
  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("slice-real-publisher");
  RedisMetaImpl writer(config, volume_name);
  RedisMetaImpl reader(config, volume_name);
  ASSERT_TRUE(writer.Initialize().ok());
  ASSERT_TRUE(reader.Initialize().ok());
  SwordFsVolume volume;
  volume.name = volume_name;
  volume.chunk_type = ChunkType::kChunkSlice;
  volume.chunk_size = kChunkSize;
  ASSERT_TRUE(writer.FormatVolume(volume).ok());
  SwordFsVolume loaded;
  loaded.name = volume_name;
  ASSERT_TRUE(reader.LoadVolume(&loaded).ok());

  ChunkMetadataPtr writer_root;
  ChunkMetadataPtr reader_root;
  ASSERT_TRUE(writer.OpenChunkMetadata(ChunkType::kChunkSlice, &writer_root).ok());
  ASSERT_TRUE(reader.OpenChunkMetadata(ChunkType::kChunkSlice, &reader_root).ok());
  auto writer_meta = std::dynamic_pointer_cast<slice::SliceChunkMetadata>(writer_root);
  auto reader_meta = std::dynamic_pointer_cast<slice::SliceChunkMetadata>(reader_root);
  ASSERT_NE(writer_meta, nullptr);
  ASSERT_NE(reader_meta, nullptr);
  swordfs::test::chunk_test::NullDataEngine objects;

  swordfs::test::RunInTestFiber([&] {
    folly::fibers::local<swordfs::utils::SwordFsContext>() = swordfs::utils::SwordFsContext{};
    SwordFsInode file;
    ASSERT_TRUE(writer.Create(kRootInodeId, "slice-publish-integration", 0644, &file).ok());
    chunk::slice::SliceChunk a(file.ino, 0, kChunkSize, writer_meta, &writer, &objects);
    ASSERT_TRUE(a.Write(5, swordfs::test::chunk_test::Buf("ABC")).ok());
    ASSERT_TRUE(a.Flush().ok()) << "first metadata publication must succeed";
    EXPECT_FALSE(a.HasPendingWrites());

    chunk::slice::SliceChunk b(file.ino, 0, kChunkSize, reader_meta, &reader, &objects);
    auto initial = folly::IOBuf::create(8);
    ASSERT_TRUE(b.Read(0, 8, initial.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(initial->data()), 8), std::string("\0\0\0\0\0ABC", 8));

    ASSERT_TRUE(b.Write(2, swordfs::test::chunk_test::Buf("XY")).ok());
    ASSERT_TRUE(b.Flush().ok()) << "second Redis client must append to existing ChunkID";
    auto after = folly::IOBuf::create(8);
    ASSERT_TRUE(a.Read(0, 8, after.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(after->data()), 8), std::string("\0\0XY\0ABC", 8));

    FileChunkSnapshot snapshot;
    ASSERT_TRUE(writer.ReadFileChunkSnapshot(file.ino, 0, &snapshot).ok());
    ASSERT_TRUE(snapshot.chunk_id.has_value());
    EXPECT_EQ(snapshot.inode.attr.size, 8U);
    std::vector<SliceRecord> records;
    ASSERT_TRUE(reader_meta->ReadRecords(*snapshot.chunk_id, &records).ok());
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records.front().pos, 5U);
    EXPECT_EQ(records.back().pos, 2U);
  });
  EXPECT_EQ(objects.put_calls, 2);
}

TEST(RedisSliceChunkMetadataTest, SliceIdsHaveIndependentVolumeScopedMonotonicCounter) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-id"));
  const redis::RedisKey other_key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-id-other"));
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.NextChunkID(), "0");
  redis.set(key.NextSliceID(), "0");
  redis.set(other_key.NextSliceID(), "0");
  auto backend = std::make_shared<RedisBackendContext>(config, 2);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata first(backend, key, 64);
  RedisSliceChunkMetadata second(backend, key, 64);
  RedisSliceChunkMetadata other(backend, other_key, 64);
  swordfs::test::RunInTestFiber([&] {
    SliceID a, b, c;
    ChunkID chunk_id;
    ASSERT_TRUE(first.AllocateSliceID(&a).ok());
    ASSERT_TRUE(second.AllocateSliceID(&b).ok());
    ASSERT_TRUE(other.AllocateSliceID(&c).ok());
    ASSERT_TRUE(first.AllocateChunkID(&chunk_id).ok());
    EXPECT_EQ(a, SliceID(1));
    EXPECT_EQ(b, SliceID(2));
    EXPECT_EQ(c, SliceID(1));
    EXPECT_EQ(chunk_id, ChunkID(1));
    EXPECT_EQ(first.AllocateSliceID(nullptr).ToErrno(), EINVAL);
  });
  EXPECT_EQ(*redis.get(key.NextSliceID()), "2");
  EXPECT_EQ(*redis.get(key.NextChunkID()), "1");
}

TEST(RedisSliceChunkMetadataTest, MissingNegativeAndExhaustedAllocationStateCannotBeReset) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-invalid-counter"));
  sw::redis::Redis redis(ConnectionOptions(config));
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata metadata(backend, key, 64);
  for (const auto &raw : {std::string(), std::string("-1"), std::string("invalid"),
                          std::to_string(std::numeric_limits<int64_t>::max())}) {
    if (raw.empty()) {
      redis.del(key.NextSliceID());
    } else {
      redis.set(key.NextSliceID(), raw);
    }
    SliceID allocated;
    swordfs::test::RunInTestFiber([&] { EXPECT_FALSE(metadata.AllocateSliceID(&allocated).ok()); });
    EXPECT_EQ(allocated, SliceID());
    if (raw.empty()) {
      EXPECT_FALSE(redis.get(key.NextSliceID()).has_value());
    } else {
      EXPECT_EQ(*redis.get(key.NextSliceID()), raw);
    }
  }
}

TEST(RedisSliceChunkMetadataTest, FirstRecordCreatesIndexAndOrderedRoundTripWithGrowingSlice) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-roundtrip"));
  auto backend = std::make_shared<RedisBackendContext>(config, 2);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata metadata(backend, key, 64);
  RedisSliceChunkMetadata remount(backend, key, 64);
  swordfs::test::RunInTestFiber([&] {
    const ChunkID first(1), missing(2);
    std::vector<SliceRecord> out = {{20, SliceID(), 0, 0, 2}};
    EXPECT_TRUE(metadata.ReadRecords(missing, &out).IsNotFound());
    EXPECT_EQ(out.size(), 1);
    EXPECT_EQ(metadata.AppendRecord(missing, {0, SliceID(1), 4, 0, 4}).ToErrno(), ENOENT);
    const std::vector<SliceRecord> committed = {
        {0, SliceID(10), 4, 0, 4}, {4, SliceID(10), 8, 4, 4}, {2, SliceID(11), 2, 0, 2}, {8, SliceID(), 0, 0, 2}};
    ASSERT_TRUE(metadata.CreateIndex(first, committed.front()).ok());
    EXPECT_EQ(metadata.CreateIndex(first, committed.front()).ToErrno(), EEXIST);
    for (size_t i = 1; i < committed.size(); ++i) {
      ASSERT_TRUE(metadata.AppendRecord(first, committed[i]).ok());
    }
    ASSERT_TRUE(remount.ReadRecords(first, &out).ok());
    ASSERT_EQ(out.size(), committed.size());
    for (size_t i = 0; i < committed.size(); ++i) {
      EXPECT_EQ(out[i].pos, committed[i].pos);
      EXPECT_EQ(out[i].id, committed[i].id);
      EXPECT_EQ(out[i].size, committed[i].size);
      EXPECT_EQ(out[i].off, committed[i].off);
      EXPECT_EQ(out[i].len, committed[i].len);
    }
    std::vector<ResolvedSlice> spans;
    ASSERT_TRUE(chunk::slice::ResolveRange(out, 64, 0, 12, &spans).ok());
    ASSERT_EQ(spans.size(), 4);
    EXPECT_EQ(spans[0].id, SliceID(10));
    EXPECT_EQ(spans[1].id, SliceID(11));
    EXPECT_EQ(spans[2].off, 4);
    EXPECT_EQ(spans[3].id, SliceID());
    EXPECT_EQ(metadata.CreateIndex(first, committed.front()).ToErrno(), EEXIST);
  });
}

TEST(RedisSliceChunkMetadataTest, CorruptStoredRecordAndWrongTypeFailClosed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-corruption"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const auto first_key = key.PrivateChunkIndex(ChunkType::kChunkSlice, "index:1");
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata metadata(backend, key, 32);
  swordfs::test::RunInTestFiber([&] { ASSERT_TRUE(metadata.CreateIndex(ChunkID(1), {0, SliceID(1), 4, 0, 4}).ok()); });
  redis.lset(first_key, 0, "truncated");
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> out = {{7, SliceID(7), 1, 0, 1}};
    EXPECT_EQ(metadata.ReadRecords(ChunkID(1), &out).ToErrno(), EIO);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].id, SliceID(7));
  });
  redis.set(first_key, "wrong-redis-type");
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> out;
    EXPECT_EQ(metadata.ReadRecords(ChunkID(1), &out).ToErrno(), EIO);
    EXPECT_EQ(metadata.AppendRecord(ChunkID(1), {0, SliceID(3), 4, 0, 4}).ToErrno(), EIO);
    EXPECT_EQ(metadata.CreateIndex(ChunkID(1), {0, SliceID(3), 4, 0, 4}).ToErrno(), EIO);
  });
  redis.del(first_key);
  swordfs::test::RunInTestFiber(
      [&] { EXPECT_EQ(metadata.AppendRecord(ChunkID(1), {0, SliceID(3), 4, 0, 4}).ToErrno(), ENOENT); });
  EXPECT_EQ(redis.exists(first_key), 0);
}

TEST(RedisSliceChunkMetadataTest, ExactBinaryRecordsAndInvalidAppends) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-codec"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const auto index_key = key.PrivateChunkIndex(ChunkType::kChunkSlice, "index:12");
  auto backend = std::make_shared<RedisBackendContext>(config, 2);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata metadata(backend, key, 32);
  swordfs::test::RunInTestFiber([&] {
    const SliceRecord first_record = {4, SliceID(7), 16, 2, 6};
    EXPECT_EQ(metadata.CreateIndex(ChunkID(), first_record).ToErrno(), EINVAL);
    EXPECT_EQ(metadata.CreateIndex(ChunkID(std::numeric_limits<uint64_t>::max()), first_record).ToErrno(), EINVAL);
    EXPECT_EQ(metadata.ReadRecords(ChunkID(12), nullptr).ToErrno(), EINVAL);
    const std::vector<SliceRecord> invalid = {
        {0, SliceID(1), 3, 0, 0}, {31, SliceID(1), 3, 0, 2},
        {0, SliceID(1), 3, 2, 2}, {0, SliceID(), 1, 0, 1},
        {0, SliceID(), 0, 1, 1},  {0, SliceID(std::numeric_limits<uint64_t>::max()), 1, 0, 1}};
    for (const auto &record : invalid) {
      EXPECT_EQ(metadata.CreateIndex(ChunkID(12), record).ToErrno(), EINVAL);
      EXPECT_EQ(metadata.AppendRecord(ChunkID(12), record).ToErrno(), EINVAL);
    }
    ASSERT_TRUE(metadata.CreateIndex(ChunkID(12), first_record).ok());
  });
  std::vector<std::string> raw;
  redis.lrange(index_key, 0, -1, std::back_inserter(raw));
  ASSERT_EQ(raw.size(), 1);
  ASSERT_EQ(raw[0].size(), 24);
  EXPECT_EQ(static_cast<unsigned char>(raw[0][0]), 4);
  EXPECT_EQ(static_cast<unsigned char>(raw[0][4]), 7);
  EXPECT_EQ(static_cast<unsigned char>(raw[0][12]), 16);
  EXPECT_EQ(static_cast<unsigned char>(raw[0][16]), 2);
  EXPECT_EQ(static_cast<unsigned char>(raw[0][20]), 6);

  redis.set(index_key, "invalid-type");
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> out;
    EXPECT_EQ(metadata.ReadRecords(ChunkID(12), &out).ToErrno(), EIO);
    EXPECT_EQ(metadata.CreateIndex(ChunkID(12), {0, SliceID(1), 4, 0, 4}).ToErrno(), EIO);
    EXPECT_EQ(metadata.AppendRecord(ChunkID(12), {0, SliceID(1), 4, 0, 4}).ToErrno(), EIO);
  });
}

TEST(RedisSliceChunkMetadataTest, IndependentInstancesAppendWithoutHistoryLengthCas) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-concurrent"));
  auto backend = std::make_shared<RedisBackendContext>(config, 4);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata first(backend, key, 32);
  RedisSliceChunkMetadata second(backend, key, 32);
  swordfs::test::RunInTestFiber([&] { ASSERT_TRUE(first.CreateIndex(ChunkID(4), {0, SliceID(1), 4, 0, 4}).ok()); });

  utils::Status first_status;
  utils::Status second_status;
  std::thread a([&] {
    swordfs::test::RunInTestFiber([&] { first_status = first.AppendRecord(ChunkID(4), {0, SliceID(50), 12, 0, 12}); });
  });
  std::thread b([&] {
    swordfs::test::RunInTestFiber([&] { second_status = second.AppendRecord(ChunkID(4), {8, SliceID(3), 12, 0, 12}); });
  });
  a.join();
  b.join();
  EXPECT_TRUE(first_status.ok()) << first_status.message();
  EXPECT_TRUE(second_status.ok()) << second_status.message();
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> records;
    ASSERT_TRUE(first.ReadRecords(ChunkID(4), &records).ok());
    ASSERT_EQ(records.size(), 3);
    std::vector<ResolvedSlice> spans;
    ASSERT_TRUE(chunk::slice::ResolveRange(records, 32, 0, 20, &spans).ok());
    ASSERT_EQ(spans.size(), 2);
    EXPECT_EQ(spans.front().pos, 0);
    EXPECT_EQ(spans.front().id, SliceID(50));
    EXPECT_EQ(spans.back().id, SliceID(3));
    EXPECT_EQ(spans.back().pos + spans.back().len, 20);
    EXPECT_EQ(records[0].id, SliceID(1));
    EXPECT_EQ(records[1].id == SliceID(50) ? records[2].id : records[1].id, SliceID(3));
    const uint32_t winning_prefix_end = records[1].id == SliceID(50) ? 8 : 12;
    EXPECT_EQ(spans.front().len, winning_prefix_end);
    EXPECT_EQ(spans.back().pos, winning_prefix_end);
  });
}

TEST(RedisSliceChunkMetadataTest, ConcurrentFirstRecordCreationPublishesExactlyOnce) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-first-record"));
  auto backend = std::make_shared<RedisBackendContext>(config, 4);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata a(backend, key, 64);
  RedisSliceChunkMetadata b(backend, key, 64);
  utils::Status first_status;
  utils::Status second_status;
  std::thread first([&] {
    swordfs::test::RunInTestFiber([&] { first_status = a.CreateIndex(ChunkID(7), {0, SliceID(1), 4, 0, 4}); });
  });
  std::thread second([&] {
    swordfs::test::RunInTestFiber([&] { second_status = b.CreateIndex(ChunkID(7), {0, SliceID(2), 4, 0, 4}); });
  });
  first.join();
  second.join();
  EXPECT_EQ(static_cast<int>(first_status.ok()) + static_cast<int>(second_status.ok()), 1);
  EXPECT_EQ(first_status.ok() ? second_status.ToErrno() : first_status.ToErrno(), EEXIST);
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> records;
    ASSERT_TRUE(a.ReadRecords(ChunkID(7), &records).ok());
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records[0].id, first_status.ok() ? SliceID(1) : SliceID(2));
  });
}

TEST(RedisSliceChunkMetadataTest, AmbiguousAllocationAndAppendRepliesAreNeverBlindlyRetried) {
  for (const auto scenario : {RedisFaultScenario::kSliceCounterExecProtocolFailureApplied,
                              RedisFaultScenario::kSliceCounterExecDisconnectNotApplied}) {
    ScriptedRedisServer server(scenario);
    auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
    auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
    RedisSliceChunkMetadata metadata(backend, redis::RedisKey(0, "slice-ambiguous-id"), 32);
    SliceID id;
    utils::Status status;
    swordfs::test::RunInTestFiber([&] { status = metadata.AllocateSliceID(&id); });
    server.Wait();
    EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
    EXPECT_EQ(id, SliceID());
    EXPECT_EQ(server.connection_count(), 1);
    EXPECT_TRUE(server.exec_received());
    EXPECT_EQ(server.mutation_applied(), scenario == RedisFaultScenario::kSliceCounterExecProtocolFailureApplied);
    EXPECT_TRUE(server.error().empty()) << server.error();
  }
  for (const auto scenario :
       {RedisFaultScenario::kSliceAppendProtocolFailureApplied, RedisFaultScenario::kSliceAppendDisconnectNotApplied}) {
    ScriptedRedisServer server(scenario);
    auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
    auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
    RedisSliceChunkMetadata metadata(backend, redis::RedisKey(0, "slice-ambiguous-list"), 32);
    utils::Status status;
    swordfs::test::RunInTestFiber([&] { status = metadata.AppendRecord(ChunkID(1), {0, SliceID(1), 4, 0, 4}); });
    server.Wait();
    EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
    EXPECT_EQ(server.connection_count(), 1);
    EXPECT_EQ(server.mutation_applied(), scenario == RedisFaultScenario::kSliceAppendProtocolFailureApplied);
    EXPECT_TRUE(server.error().empty()) << server.error();
  }
}

TEST(RedisSliceChunkMetadataTest, FormatBootstrapsCounterAndOpensTypedSliceMetadata) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const auto volume_name = swordfs::test::UniqueRedisTestNamespace("slice-formatted");
  const redis::RedisKey key(config.db, volume_name);
  RedisMetaOps ops(config, volume_name);
  SwordFsVolume volume;
  volume.name = volume_name;
  volume.chunk_type = ChunkType::kChunkSlice;
  ASSERT_TRUE(ops.FormatVolume(volume).ok());
  sw::redis::Redis redis(ConnectionOptions(config));
  ASSERT_EQ(*redis.get(key.NextSliceID()), "0");
  ASSERT_EQ(*redis.get(key.NextChunkID()), "0");

  ChunkMetadataPtr base;
  ASSERT_TRUE(ops.OpenChunkMetadata(ChunkType::kChunkSlice, &base).ok());
  auto typed = std::dynamic_pointer_cast<slice::SliceChunkMetadata>(base);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->Type(), ChunkType::kChunkSlice);
  EXPECT_EQ(ops.OpenChunkMetadata(ChunkType::kCow, &base).ToErrno(), EINVAL);
  swordfs::test::RunInTestFiber([&] {
    SliceID first;
    ASSERT_TRUE(typed->AllocateSliceID(&first).ok());
    EXPECT_EQ(first, SliceID(1));
    ChunkID chunk_id;
    ASSERT_TRUE(typed->AllocateChunkID(&chunk_id).ok());
    ASSERT_TRUE(typed->CreateIndex(chunk_id, {0, first, 4, 0, 4}).ok());
    std::vector<SliceRecord> records;
    ASSERT_TRUE(typed->ReadRecords(chunk_id, &records).ok());
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records[0].id, first);
  });
  EXPECT_EQ(*redis.get(key.NextSliceID()), "1");
}

TEST(RedisSliceChunkMetadataTest, RejectsMissingVolumeConfigurationBeforeOpeningSliceIndex) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  RedisMetaOps ops(config, swordfs::test::UniqueRedisTestNamespace("slice-no-volume"));
  ChunkMetadataPtr metadata;
  EXPECT_EQ(ops.OpenChunkMetadata(ChunkType::kChunkSlice, &metadata).ToErrno(), EINVAL);
  EXPECT_EQ(metadata, nullptr);
}

TEST(RedisSliceChunkMetadataTest, RedisListAndCounterAdaptersRejectNullOutputs) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  RedisMetaClient client(config);
  EXPECT_EQ(client.IncrExistingNonNegative("ignored", nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(client.LRange("ignored", nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(client.Transact([](RedisKvTxn &txn) { return txn.ListPrefix("ignored", nullptr); }).ToErrno(), EINVAL);
  const auto transactional_key = swordfs::test::UniqueRedisTestNamespace("slice-no-partial-init");
  std::vector<std::string> prefix;
  EXPECT_EQ(client
                .Transact([&](RedisKvTxn &txn) {
                  auto status = txn.Set(transactional_key, "uncommitted");
                  if (!status.ok()) {
                    return status;
                  }
                  return txn.ListPrefix(transactional_key, &prefix);
                })
                .ToErrno(),
            EINVAL);
  sw::redis::Redis redis(ConnectionOptions(config));
  EXPECT_FALSE(redis.get(transactional_key).has_value());
}

TEST(RedisSliceChunkMetadataTest, NativeAppendErrorReplyIsNotAnAmbiguousCommit) {
  ScriptedRedisServer server(RedisFaultScenario::kSliceAppendReplyError);
  auto backend = std::make_shared<RedisBackendContext>(ScriptedConfig(server), 1);
  auto shutdown = folly::makeGuard([&] { backend->Shutdown(); });
  RedisSliceChunkMetadata metadata(backend, redis::RedisKey(0, "slice-error-reply"), 32);
  utils::Status status;
  swordfs::test::RunInTestFiber([&] { status = metadata.AppendRecord(ChunkID(1), {0, SliceID(1), 4, 0, 4}); });
  server.Wait();
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_FALSE(status.IsOutcomeUnknown());
  EXPECT_TRUE(server.error().empty()) << server.error();
  EXPECT_EQ(server.connection_count(), 1);
}

}  // namespace
}  // namespace swordfs::metadata
