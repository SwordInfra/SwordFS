// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::metadata {

TEST(RedisMetaConfigTest, ParsesHostWithDefaults) {
  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl("redis://localhost", &config);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(config.host, "localhost");
  EXPECT_EQ(config.port, 6379);
  EXPECT_EQ(config.db, 0);
  EXPECT_FALSE(config.username.has_value());
  EXPECT_FALSE(config.password.has_value());
  EXPECT_EQ(config.connect_timeout, std::chrono::seconds(2));
  EXPECT_EQ(config.socket_timeout, std::chrono::seconds(5));
  EXPECT_EQ(config.pool_size, 8);
  EXPECT_EQ(config.pool_wait_timeout, std::chrono::seconds(1));
  EXPECT_EQ(config.retry_attempts, 3);
  EXPECT_EQ(config.retry_backoff, std::chrono::milliseconds(20));

  status = ParseRedisMetaUrl("redis://localhost/", &config);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(config.db, 0);
}

TEST(RedisMetaConfigTest, ParsesAuthenticationPortAndDatabase) {
  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl("redis://user:secret@example.com:6380/3", &config);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(config.host, "example.com");
  EXPECT_EQ(config.port, 6380);
  EXPECT_EQ(config.db, 3);
  ASSERT_TRUE(config.username.has_value());
  ASSERT_TRUE(config.password.has_value());
  EXPECT_EQ(*config.username, "user");
  EXPECT_EQ(*config.password, "secret");
}

TEST(RedisMetaConfigTest, ParsesIpv6Host) {
  RedisMetaConfig config;
  const auto status = ParseRedisMetaUrl("redis://[::1]", &config);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(config.host, "::1");
  EXPECT_EQ(config.port, 6379);

  const auto port_status = ParseRedisMetaUrl("redis://[2001:db8::1]:6380/2", &config);
  ASSERT_TRUE(port_status.ok()) << port_status.message();
  EXPECT_EQ(config.host, "2001:db8::1");
  EXPECT_EQ(config.port, 6380);
  EXPECT_EQ(config.db, 2);
}

TEST(RedisMetaConfigTest, ParsesClientOptions) {
  RedisMetaConfig config;
  const auto status = ParseRedisMetaUrl(
      "redis://localhost/"
      "0?connect_timeout=250ms&socket_timeout=3s&pool_size=16&pool_wait_timeout=2s&retry_attempts=5&retry_backoff=40ms",
      &config);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(config.connect_timeout, std::chrono::milliseconds(250));
  EXPECT_EQ(config.socket_timeout, std::chrono::seconds(3));
  EXPECT_EQ(config.pool_size, 16);
  EXPECT_EQ(config.pool_wait_timeout, std::chrono::seconds(2));
  EXPECT_EQ(config.retry_attempts, 5);
  EXPECT_EQ(config.retry_backoff, std::chrono::milliseconds(40));
}

TEST(RedisMetaConfigTest, RejectsInvalidClientOptions) {
  for (const auto url :
       {"redis://localhost?connect_timeout=0s", "redis://localhost?socket_timeout=-1s", "redis://localhost?pool_size=0",
        "redis://localhost?pool_wait_timeout=0s", "redis://localhost?retry_attempts=0",
        "redis://localhost?retry_backoff=abc", "redis://localhost?unknown=1", "redis://localhost?pool_size=abc"}) {
    RedisMetaConfig config;
    auto status = ParseRedisMetaUrl(url, &config);
    EXPECT_FALSE(status.ok()) << url;
  }
}

TEST(RedisMetaConfigTest, ParsesPasswordOnlyAuthentication) {
  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl("redis://:secret@127.0.0.1/1", &config);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(config.username.has_value());
  ASSERT_TRUE(config.password.has_value());
  EXPECT_EQ(*config.password, "secret");
}

TEST(RedisMetaConfigTest, RejectsInvalidUrls) {
  for (const auto url :
       {"memory://local", "redis://", "redis://:6379", "redis://host:0", "redis://host:00", "redis://host:000",
        "redis://host:", "redis://host:70000", "redis://host/not-a-db", "redis://host?x=1"}) {
    RedisMetaConfig config;
    auto status = ParseRedisMetaUrl(url, &config);
    EXPECT_FALSE(status.ok()) << url;
  }
}

}  // namespace swordfs::metadata
