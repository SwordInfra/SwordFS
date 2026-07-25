// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "storage/StorageUrl.hpp"

using swordfs::utils::StorageUrl;

TEST(StorageUrlTest, ParseMemoryBackend) {
  StorageUrl url;
  EXPECT_TRUE(StorageUrl::Parse("memory://local", &url));
  EXPECT_EQ(url.scheme, "memory");
  EXPECT_EQ(url.host, "local");
  EXPECT_TRUE(url.path.empty());
}

TEST(StorageUrlTest, ParseRedis) {
  StorageUrl url;
  EXPECT_TRUE(StorageUrl::Parse("redis://127.0.0.1:6379/0", &url));
  EXPECT_EQ(url.scheme, "redis");
  EXPECT_EQ(url.host, "127.0.0.1:6379");
  EXPECT_EQ(url.path, "/0");
}

TEST(StorageUrlTest, ParseS3) {
  StorageUrl url;
  EXPECT_TRUE(StorageUrl::Parse("s3://mybucket.s3.amazonaws.com/chunks", &url));
  EXPECT_EQ(url.scheme, "s3");
  EXPECT_EQ(url.host, "mybucket.s3.amazonaws.com");
  EXPECT_EQ(url.path, "/chunks");
}

TEST(StorageUrlTest, ParseS3NoPath) {
  StorageUrl url;
  EXPECT_TRUE(StorageUrl::Parse("s3://my-bucket", &url));
  EXPECT_EQ(url.scheme, "s3");
  EXPECT_EQ(url.host, "my-bucket");
  EXPECT_TRUE(url.path.empty());
}

TEST(StorageUrlTest, ParseEmpty) {
  StorageUrl url;
  EXPECT_FALSE(StorageUrl::Parse("", &url));
}

TEST(StorageUrlTest, ParseNoScheme) {
  StorageUrl url;
  EXPECT_FALSE(StorageUrl::Parse("://host/path", &url));
}

TEST(StorageUrlTest, ParseNoHost) {
  StorageUrl url;
  EXPECT_FALSE(StorageUrl::Parse("redis://", &url));
}

TEST(StorageUrlTest, ParseNullOut) {
  EXPECT_FALSE(StorageUrl::Parse("s3://bucket", nullptr));
}

TEST(StorageUrlTest, ToStringRoundtrip) {
  StorageUrl url;
  ASSERT_TRUE(StorageUrl::Parse("s3://mybucket.s3.amazonaws.com/chunks", &url));
  EXPECT_EQ(url.ToString(), "s3://mybucket.s3.amazonaws.com/chunks");
}

TEST(StorageUrlTest, ToStringMemory) {
  StorageUrl url;
  ASSERT_TRUE(StorageUrl::Parse("memory://local", &url));
  EXPECT_EQ(url.ToString(), "memory://local");
}

TEST(StorageUrlTest, Empty) {
  StorageUrl url;
  EXPECT_TRUE(url.empty());
  EXPECT_TRUE(url.scheme.empty());
}
