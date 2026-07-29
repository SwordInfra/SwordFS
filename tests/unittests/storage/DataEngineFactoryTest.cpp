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
using swordfs::utils::Status;
using swordfs::volume::VolumeConfig;

namespace {

VolumeConfig MakeVol(const std::string &bucket, const std::string &region) {
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
// CreateDataEngine — empty / error cases
// ────────────────────────────────────────────────────────────────

TEST(DataEngineFactoryTest, EmptyBucketReturnsError) {
  VolumeConfig vol = MakeVol("", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(vol, &engine);
  EXPECT_FALSE(st.ok());
}

TEST(DataEngineFactoryTest, InvalidBucketUrlReturnsError) {
  VolumeConfig vol = MakeVol("not-a-valid-url", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(vol, &engine);
  EXPECT_FALSE(st.ok());
}

TEST(DataEngineFactoryTest, UnknownSchemeReturnsError) {
  VolumeConfig vol = MakeVol("ftp://host/bucket", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(vol, &engine);
  EXPECT_FALSE(st.ok());
}

// ────────────────────────────────────────────────────────────────
// CreateDataEngine — S3 engine creation
// ────────────────────────────────────────────────────────────────

TEST(DataEngineFactoryTest, S3SchemeCreatesEngine) {
  VolumeConfig vol = MakeVol("s3://myhost.example.com/mybucket", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(vol, &engine);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(engine, nullptr);
  EXPECT_GT(engine->Limits().max_chunk_size, 0U);
}

TEST(DataEngineFactoryTest, S3EnginePropagatesRegion) {
  VolumeConfig vol = MakeVol("s3://myhost.example.com/bucket", "us-west-2");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(vol, &engine);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(engine, nullptr);
  EXPECT_GT(engine->Limits().max_chunk_size, 0U);
}

TEST(DataEngineFactoryTest, S3SchemeWithPrefix) {
  VolumeConfig vol = MakeVol("s3://myhost.example.com/bucket/prefix/path", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(vol, &engine);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(engine, nullptr);
}
