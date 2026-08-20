// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for DataEngineFactory.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "config/ConfigCenter.hpp"
#include "storage/DataEngineFactory.hpp"
#include "storage/IDataEngine.hpp"
#include "volume/VolumeConfig.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::config::ConfigCenter;
using swordfs::storage::CreateDataEngine;
using swordfs::storage::IDataEngine;
using swordfs::utils::Status;
using swordfs::volume::VolumeConfig;
using swordfs::volume::VolumeImpl;

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

// Seed the VolumeImpl singleton the same way production does: write
// the desired values into ConfigCenter, then let CreateFrom build the
// VolumeConfig.  S3DataEngine::Initialize() reads the config back from
// VolumeImpl::Instance().config(), so the two must stay in sync.
void SeedVolume(const std::string &bucket, const std::string &region) {
  VolumeImpl::Initialize();
  ConfigCenter &cfg = ConfigCenter::Instance();
  cfg.Initialize();
  cfg.set_meta_url("memory://local");
  cfg.set_bucket_url(bucket);
  cfg.set_storage_region(region);
  ASSERT_TRUE(VolumeImpl::Instance().CreateFrom(cfg).ok());
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

TEST(DataEngineFactoryTest, S3UrlMissingBucketName) {
  // SeedVolume goes through CreateFrom, so Initialize() pulls the
  // bucket URL from the singleton and fails with "missing bucket name".
  SeedVolume("s3://endpoint.example.com", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(VolumeImpl::Instance().config(), &engine);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("missing bucket name"), std::string::npos)
      << st.message();
  VolumeImpl::Instance().Shutdown();
}
// ────────────────────────────────────────────────────────────────
// CreateDataEngine — S3 engine creation
// ────────────────────────────────────────────────────────────────

TEST(DataEngineFactoryTest, S3SchemeCreatesEngine) {
  SeedVolume("s3://myhost.example.com/mybucket", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(VolumeImpl::Instance().config(), &engine);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(engine, nullptr);
  EXPECT_FALSE(engine->Limits().supports_multipart);
  VolumeImpl::Instance().Shutdown();
}

TEST(DataEngineFactoryTest, S3EnginePropagatesRegion) {
  SeedVolume("s3://myhost.example.com/bucket", "us-west-2");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(VolumeImpl::Instance().config(), &engine);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(engine, nullptr);
  EXPECT_FALSE(engine->Limits().supports_multipart);
  VolumeImpl::Instance().Shutdown();
}

TEST(DataEngineFactoryTest, S3SchemeWithPrefix) {
  SeedVolume("s3://myhost.example.com/bucket/prefix/path", "auto");
  std::unique_ptr<IDataEngine> engine;
  Status st = CreateDataEngine(VolumeImpl::Instance().config(), &engine);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(engine, nullptr);
  VolumeImpl::Instance().Shutdown();
}
