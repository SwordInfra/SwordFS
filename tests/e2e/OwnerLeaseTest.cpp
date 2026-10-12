// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "tests/e2e/Fixture.hpp"

namespace swordfs::e2e {
namespace {

using namespace std::chrono_literals;

constexpr auto kLeaseOptions =
    "--enable-lock-owner-lease --lock-lease-ttl 5s --lock-lease-renew-interval 1s "
    "--lock-lease-safety-margin 2s --lock-lease-io-timeout 250ms";

struct OwnerLeaseRedis {
  OwnerLeaseRedis(const char *url, const std::string &volume_name) : key(0, volume_name) {
    metadata::RedisMetaConfig config;
    const auto status = metadata::ParseRedisMetaUrl(url, &config);
    if (!status.ok()) {
      throw std::runtime_error(status.message());
    }
    key = metadata::redis::RedisKey(config.db, volume_name);
    sw::redis::ConnectionOptions options;
    options.host = config.host;
    options.port = config.port;
    options.db = config.db;
    if (config.username.has_value()) {
      options.user = *config.username;
    }
    if (config.password.has_value()) {
      options.password = *config.password;
    }
    redis = std::make_unique<sw::redis::Redis>(options);
  }

  metadata::redis::RedisKey key;
  std::unique_ptr<sw::redis::Redis> redis;
};

TEST(OwnerLeaseE2ETest, DeletedLeaseDoesNotUnmountOrdinaryFuseFilesystem) {
  Fixture fixture;
  fixture.SetExtraMountOptions(kLeaseOptions);
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto current = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(current.has_value());
  const auto generation = std::stoull(*current);
  ASSERT_GT(generation, 0U);
  ASSERT_GT(authority.redis->pttl(authority.key.OwnerLease(generation)), 0);

  // Server-side revocation is immediately authoritative. This test checks
  // ordinary FUSE availability while the lease key is absent; the native
  // Redis unit test separately verifies heartbeat detection and terminality.
  authority.redis->del(authority.key.OwnerLease(generation));
  ASSERT_EQ(authority.redis->exists(authority.key.OwnerLease(generation)), 0);
  struct stat st{};
  EXPECT_TRUE(fixture.IsMounted());
  EXPECT_EQ(fixture.Stat("", &st), 0);
  fixture.TearDown();
}

TEST(OwnerLeaseE2ETest, ExplicitUnmountDeletesOnlyItsExactOwnerLease) {
  Fixture fixture;
  fixture.SetExtraMountOptions(kLeaseOptions);
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto current = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(current.has_value());
  const auto generation = std::stoull(*current);
  ASSERT_GT(generation, 0U);
  ASSERT_GT(authority.redis->pttl(authority.key.OwnerLease(generation)), 0);

  fixture.TearDown();
  EXPECT_EQ(authority.redis->exists(authority.key.OwnerLease(generation)), 0);
  EXPECT_EQ(authority.redis->get(authority.key.OwnerGeneration()), current);
}

TEST(OwnerLeaseE2ETest, InvalidLeaseTimingsDoNotPreventOrdinaryMount) {
  Fixture fixture;
  fixture.SetExtraMountOptions(
      "--enable-lock-owner-lease --lock-lease-ttl 5s --lock-lease-renew-interval 4s "
      "--lock-lease-safety-margin 2s --lock-lease-io-timeout 1s");
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  EXPECT_EQ(authority.redis->get(authority.key.OwnerGeneration()), "0");
  struct stat st{};
  EXPECT_TRUE(fixture.IsMounted());
  EXPECT_EQ(fixture.Stat("", &st), 0);
  fixture.TearDown();
}

TEST(OwnerLeaseE2ETest, CleanRemountAllocatesFreshGeneration) {
  Fixture fixture;
  fixture.SetExtraMountOptions(kLeaseOptions);
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto first = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(first.has_value());
  const auto first_generation = std::stoull(*first);
  ASSERT_GT(authority.redis->pttl(authority.key.OwnerLease(first_generation)), 0);

  ASSERT_TRUE(fixture.Remount());
  const auto next = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(next.has_value());
  const auto next_generation = std::stoull(*next);
  EXPECT_GT(next_generation, first_generation);
  EXPECT_EQ(authority.redis->exists(authority.key.OwnerLease(first_generation)), 0);
  EXPECT_GT(authority.redis->pttl(authority.key.OwnerLease(next_generation)), 0);
  fixture.TearDown();
}

}  // namespace
}  // namespace swordfs::e2e
