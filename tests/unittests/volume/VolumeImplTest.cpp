// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "config/ConfigCenter.hpp"
#include "metadata/mem/VolumeFile.hpp"
#include "storage/IDataEngine.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::config::ConfigCenter;
using swordfs::metadata::mem::VolumeFile;
using swordfs::utils::Status;
using swordfs::volume::VolumeImpl;

class VolumeImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (access("/etc/swordfs", W_OK) != 0 && access("/etc", W_OK) != 0) {
      GTEST_SKIP() << "/etc/swordfs is not writable";
    }
    tmpdir_ = "/tmp/swordfs_volimpl_test_" + std::to_string(::getpid());
    std::system(("mkdir -p " + tmpdir_).c_str());
  }
  void TearDown() override {
    std::system(("rm -rf " + tmpdir_).c_str());
  }

  ConfigCenter makeConfig(const std::string &meta_url, const std::string &vol_name = "testvol",
                          const std::string &bucket_url = "") {
    ConfigCenter cfg;
    cfg.set_meta_url(meta_url);
    cfg.set_volume(vol_name + "-" + std::to_string(::getpid()));
    cfg.set_bucket_url(bucket_url);
    return std::move(cfg);
  }

  std::string tmpdir_;
};

// ── CreateFrom ──────────────────────────────────────────────────────

TEST_F(VolumeImplTest, CreateFromSucceeds) {
  auto cfg = makeConfig("memory://local");
  VolumeImpl vol;
  EXPECT_TRUE(vol.CreateFrom(cfg).ok());
}

TEST_F(VolumeImplTest, CreateFromRedisEngine) {
  const char *redis_url = std::getenv("SWORDFS_REDIS_TEST_URL");
  if (redis_url == nullptr) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  auto cfg = makeConfig(redis_url, "redis-" + tmpdir_, "s3://endpoint.example.com/bucket");
  cfg.set_storage_backend("s3");
  cfg.set_storage_region("us-east-1");

  VolumeImpl::Initialize();
  Status status = VolumeImpl::Instance().CreateFrom(cfg);
  ASSERT_TRUE(status.ok()) << status.message();

  auto mount_cfg = makeConfig(redis_url, "redis-" + tmpdir_);
  VolumeImpl::Initialize();
  status = VolumeImpl::Instance().LoadFrom(mount_cfg);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_NE(VolumeImpl::Instance().data_engine(), nullptr);
  EXPECT_FALSE(VolumeFile{tmpdir_}.Exists());

  VolumeImpl::Initialize();
  status = VolumeImpl::Instance().CreateFrom(cfg);
  EXPECT_TRUE(status.IsAlreadyExists()) << status.message();
}

TEST_F(VolumeImplTest, LoadFromS3Engine) {
  auto cfg = makeConfig("memory://local", "testvol", "s3://myhost.example.com/mybucket");
  cfg.set_storage_region("us-west-2");

  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(cfg).ok());

  VolumeImpl::Initialize();
  Status st = VolumeImpl::Instance().LoadFrom(cfg);
  ASSERT_TRUE(st.ok()) << st.message();
  ASSERT_NE(VolumeImpl::Instance().data_engine(), nullptr);
  EXPECT_FALSE(VolumeImpl::Instance().data_engine()->Limits().supports_multipart);
  VolumeImpl::Instance().Shutdown();
}

TEST_F(VolumeImplTest, LoadFromInvalidBucketUrl) {
  auto cfg = makeConfig("memory://local", "testvol", "not-a-valid-url");

  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(cfg).ok());

  VolumeImpl::Initialize();
  Status st = VolumeImpl::Instance().LoadFrom(cfg);
  EXPECT_FALSE(st.ok());
}

TEST_F(VolumeImplTest, LoadFromUnknownDataEngine) {
  auto cfg = makeConfig("memory://local", "testvol", "ftp://host/bucket");

  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(cfg).ok());

  VolumeImpl::Initialize();
  Status st = VolumeImpl::Instance().LoadFrom(cfg);
  EXPECT_TRUE(st.IsNotSupported()) << st.message();
}

TEST_F(VolumeImplTest, LoadFromS3UrlMissingBucketName) {
  auto cfg = makeConfig("memory://local", "testvol", "s3://endpoint.example.com");

  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(cfg).ok());

  VolumeImpl::Initialize();
  Status st = VolumeImpl::Instance().LoadFrom(cfg);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("missing bucket name"), std::string::npos) << st.message();
}

TEST_F(VolumeImplTest, CreateFromVolumeAlreadyExists) {
  auto cfg = makeConfig("memory://local", tmpdir_);
  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(cfg).ok());

  // Second format on the same path must fail.
  VolumeImpl vol2;
  Status st = vol2.CreateFrom(cfg);
  EXPECT_FALSE(st.ok());
}

// ── LoadFrom ────────────────────────────────────────────────────────

TEST_F(VolumeImplTest, LoadFromSucceeds) {
  auto cfg = makeConfig("memory://local", tmpdir_);
  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(cfg).ok());

  VolumeImpl vol2;
  Status st = vol2.LoadFrom(cfg);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(VolumeImplTest, LoadFromUnsupportedEngine) {
  auto cfg = makeConfig("redis://localhost:6379/0", tmpdir_);
  VolumeImpl vol;
  Status st = vol.LoadFrom(cfg);
  EXPECT_FALSE(st.ok());
}

TEST_F(VolumeImplTest, LoadFromMissingFile) {
  auto cfg = makeConfig("memory://local", "nonexistent_vol_impl_test");
  VolumeImpl vol;
  Status st = vol.LoadFrom(cfg);
  EXPECT_FALSE(st.ok());
}
