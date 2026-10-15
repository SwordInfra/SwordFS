// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <cerrno>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/slice/SliceRecord.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaTestSupport.hpp"
#include "metadata/redis/RedisSliceChunkMetadata.hpp"
#include "metadata/redis/RedisTestUtils.hpp"

namespace swordfs::metadata {
namespace {

using chunk::slice::ResolvedSlice;
using chunk::slice::SliceID;
using chunk::slice::SliceRecord;

TEST(RedisSliceChunkMetadataTest, SliceIdsHaveIndependentVolumeScopedMonotonicCounter) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-id"));
  const redis::RedisKey other_key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-id-other"));
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.set(key.NextSliceID(), "0");
  redis.set(other_key.NextSliceID(), "0");
  auto backend = std::make_shared<RedisBackendContext>(config, 2);
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
  backend->Shutdown();
}

TEST(RedisSliceChunkMetadataTest, MissingNegativeAndExhaustedAllocationStateCannotBeReset) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-invalid-counter"));
  sw::redis::Redis redis(ConnectionOptions(config));
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
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
  backend->Shutdown();
}

TEST(RedisSliceChunkMetadataTest, EmptyIndexAndOrderedRoundTripWithGrowingSlice) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-roundtrip"));
  auto backend = std::make_shared<RedisBackendContext>(config, 2);
  RedisSliceChunkMetadata metadata(backend, key, 64);
  RedisSliceChunkMetadata remount(backend, key, 64);
  swordfs::test::RunInTestFiber([&] {
    const ChunkID first(1), missing(2);
    std::vector<SliceRecord> out = {{20, SliceID(), 0, 0, 2}};
    EXPECT_TRUE(metadata.ReadRecords(missing, &out).IsNotFound());
    EXPECT_EQ(out.size(), 1);
    EXPECT_EQ(metadata.AppendRecord(missing, {0, SliceID(1), 4, 0, 4}).ToErrno(), ENOENT);
    ASSERT_TRUE(metadata.CreateIndex(first).ok());
    ASSERT_TRUE(metadata.CreateIndex(first).ok());
    ASSERT_TRUE(remount.ReadRecords(first, &out).ok());
    EXPECT_TRUE(out.empty());
    const std::vector<SliceRecord> committed = {
        {0, SliceID(10), 4, 0, 4}, {4, SliceID(10), 8, 4, 4}, {2, SliceID(11), 2, 0, 2}, {8, SliceID(), 0, 0, 2}};
    for (const auto &record : committed) {
      ASSERT_TRUE(metadata.AppendRecord(first, record).ok());
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
    EXPECT_EQ(metadata.CreateIndex(first).ToErrno(), EEXIST);
  });
  backend->Shutdown();
}

TEST(RedisSliceChunkMetadataTest, CorruptStoredRecordAndWrongHeaderFailClosed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  const redis::RedisKey key(config.db, swordfs::test::UniqueRedisTestNamespace("slice-corruption"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const auto first_key = key.PrivateChunkIndex(ChunkType::kChunkSlice, "index:1");
  const auto second_key = key.PrivateChunkIndex(ChunkType::kChunkSlice, "index:2");
  auto backend = std::make_shared<RedisBackendContext>(config, 1);
  RedisSliceChunkMetadata metadata(backend, key, 32);
  swordfs::test::RunInTestFiber([&] {
    ASSERT_TRUE(metadata.CreateIndex(ChunkID(1)).ok());
    ASSERT_TRUE(metadata.AppendRecord(ChunkID(1), {0, SliceID(1), 4, 0, 4}).ok());
  });
  redis.lset(first_key, 1, "truncated");
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> out = {{7, SliceID(7), 1, 0, 1}};
    EXPECT_EQ(metadata.ReadRecords(ChunkID(1), &out).ToErrno(), EIO);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].id, SliceID(7));
  });
  redis.del(first_key);
  // A foreign header must not turn a different ChunkID's index into a valid one.
  swordfs::test::RunInTestFiber([&] { ASSERT_TRUE(metadata.CreateIndex(ChunkID(1)).ok()); });
  std::vector<std::string> elements;
  redis.lrange(first_key, 0, -1, std::back_inserter(elements));
  ASSERT_EQ(elements.size(), 1);
  redis.rpush(second_key, elements[0]);
  swordfs::test::RunInTestFiber([&] {
    std::vector<SliceRecord> out;
    EXPECT_EQ(metadata.ReadRecords(ChunkID(2), &out).ToErrno(), EIO);
    EXPECT_EQ(metadata.AppendRecord(ChunkID(2), {0, SliceID(3), 4, 0, 4}).ToErrno(), EIO);
    EXPECT_EQ(metadata.CreateIndex(ChunkID(2)).ToErrno(), EIO);
  });
  backend->Shutdown();
}

}  // namespace
}  // namespace swordfs::metadata
