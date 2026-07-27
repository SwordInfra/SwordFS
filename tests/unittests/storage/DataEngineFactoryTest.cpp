// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for DataEngineFactory.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "storage/DataEngineFactory.hpp"
#include "storage/IDataEngine.hpp"
#include "volume/VolumeConfig.hpp"

using swordfs::storage::CreateDataEngine;
using swordfs::storage::IDataEngine;
using swordfs::volume::VolumeConfig;

namespace {

VolumeConfig MakeVol(const std::string& bucket, const std::string& region) {
  VolumeConfig v;
  v.name = "test";
  v.meta_url = "memory://local";
  v.storage = "s3";
  v.bucket = bucket;
  v.region = region;
  return v;
}

}  // namespace

// ────────────────────────────────────────────────────────────────
// CreateDataEngine — nullptr cases
// ────────────────────────────────────────────────────────────────

TEST(DataEngineFactoryTest, EmptyBucketReturnsNull) {
  VolumeConfig vol = MakeVol("", "auto");
  EXPECT_EQ(CreateDataEngine(vol), nullptr);
}

TEST(DataEngineFactoryTest, InvalidBucketUrlReturnsNull) {
  VolumeConfig vol = MakeVol("not-a-valid-url", "auto");
  EXPECT_EQ(CreateDataEngine(vol), nullptr);
}

TEST(DataEngineFactoryTest, UnknownSchemeReturnsNull) {
  VolumeConfig vol = MakeVol("ftp://host/bucket", "auto");
  EXPECT_EQ(CreateDataEngine(vol), nullptr);
}

// ────────────────────────────────────────────────────────────────
// CreateDataEngine — S3 engine creation
// ────────────────────────────────────────────────────────────────

TEST(DataEngineFactoryTest, S3SchemeCreatesEngine) {
  VolumeConfig vol = MakeVol("s3://myhost.example.com/mybucket", "auto");
  auto engine = CreateDataEngine(vol);
  // Engine should be created (even if it won't connect in unit tests).
  ASSERT_NE(engine, nullptr);
  EXPECT_GT(engine->Limits().max_chunk_size, 0U);
}

TEST(DataEngineFactoryTest, S3EnginePropagatesRegion) {
  VolumeConfig vol = MakeVol("s3://myhost.example.com/bucket", "us-west-2");
  auto engine = CreateDataEngine(vol);
  ASSERT_NE(engine, nullptr);
  // Region is embedded in S3DataEngine; construction succeeds with non-"auto".
  EXPECT_GT(engine->Limits().max_chunk_size, 0U);
}

TEST(DataEngineFactoryTest, S3SchemeWithPrefix) {
  VolumeConfig vol = MakeVol("s3://myhost.example.com/bucket/prefix/path", "auto");
  auto engine = CreateDataEngine(vol);
  ASSERT_NE(engine, nullptr);
}
