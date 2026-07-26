// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "volume/VolumeConfig.hpp"

using swordfs::storage::VolumeConfig;
using swordfs::utils::Status;

class VolumeConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmpdir_ = "/tmp/swordfs_vol_test_" + std::to_string(::getpid());
    std::system(("mkdir -p " + tmpdir_).c_str());
  }
  void TearDown() override {
    std::system(("rm -rf " + tmpdir_).c_str());
  }
  std::string tmpdir_;
};

TEST_F(VolumeConfigTest, ToJsonAndFromJsonRoundtrip) {
  VolumeConfig cfg;
  cfg.name = "testvol";
  cfg.uuid = VolumeConfig::GenerateUUID();
  cfg.meta_url = "memory://local";
  cfg.bucket = "s3://mybucket.s3.amazonaws.com/chunks";

  std::string json = cfg.ToJson();
  EXPECT_GT(json.size(), 0);
  EXPECT_NE(json.find("\"name\""), std::string::npos);
  EXPECT_NE(json.find("\"uuid\""), std::string::npos);
  EXPECT_NE(json.find("memory://local"), std::string::npos);

  VolumeConfig restored;
  Status st = VolumeConfig::FromJson(json, &restored);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(restored.name, cfg.name);
  EXPECT_EQ(restored.uuid, cfg.uuid);
  EXPECT_EQ(restored.meta_url, cfg.meta_url);
  EXPECT_EQ(restored.bucket, cfg.bucket);
}

TEST_F(VolumeConfigTest, GenerateUUIDFormat) {
  std::string uuid = VolumeConfig::GenerateUUID();
  // UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx (36 chars with dashes)
  EXPECT_EQ(uuid.size(), 36);
  EXPECT_EQ(uuid[14], '4');  // version 4
}

TEST_F(VolumeConfigTest, WriteAndReadFileRoundtrip) {
  VolumeConfig cfg;
  cfg.name = "roundtrip-vol";
  cfg.uuid = VolumeConfig::GenerateUUID();
  cfg.meta_url = "redis://127.0.0.1:6379/0";
  cfg.bucket = "";

  Status st = cfg.WriteToFile(tmpdir_);
  EXPECT_TRUE(st.ok()) << st.message();

  VolumeConfig restored;
  st = VolumeConfig::ReadFromFile(tmpdir_, &restored);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(restored.name, cfg.name);
  EXPECT_EQ(restored.uuid, cfg.uuid);
  EXPECT_EQ(restored.meta_url, cfg.meta_url);
  EXPECT_EQ(restored.bucket, cfg.bucket);
}

TEST_F(VolumeConfigTest, ReadFromFileMissing) {
  VolumeConfig cfg;
  Status st = VolumeConfig::ReadFromFile("/tmp/nonexistent_vol_test_dir", &cfg);
  EXPECT_FALSE(st.ok());
}

TEST_F(VolumeConfigTest, FromJsonInvalid) {
  VolumeConfig cfg;
  Status st = VolumeConfig::FromJson("not valid json {{{", &cfg);
  EXPECT_FALSE(st.ok());
}

TEST_F(VolumeConfigTest, FromJsonEmptyObject) {
  // Empty JSON object — implementation may require at least some fields.
  VolumeConfig cfg;
  Status st = VolumeConfig::FromJson("{}", &cfg);
  // Either ok (empty is valid) or error (requires fields) — both are reasonable.
  if (st.ok()) {
    EXPECT_TRUE(cfg.name.empty());
  }
}
