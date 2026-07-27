// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "config/ConfigCenter.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::config::ConfigCenter;
using swordfs::utils::Status;
using swordfs::volume::VolumeImpl;

class VolumeImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmpdir_ = "/tmp/swordfs_volimpl_test_" + std::to_string(::getpid());
    std::system(("mkdir -p " + tmpdir_).c_str());
  }
  void TearDown() override {
    std::system(("rm -rf " + tmpdir_).c_str());
  }

  ConfigCenter makeConfig(const std::string& meta_url,
                          const std::string& vol_path,
                          const std::string& vol_name = "testvol") {
    ConfigCenter cfg;
    cfg.set_meta_url(meta_url);
    cfg.set_volume_config_path(vol_path);
    cfg.set_volume(vol_name);
    return std::move(cfg);
  }

  std::string tmpdir_;
};

// ── CreateFrom ──────────────────────────────────────────────────────

TEST_F(VolumeImplTest, CreateFromSucceeds) {
  auto cfg = makeConfig("memory://local", tmpdir_);
  VolumeImpl vol;
  EXPECT_TRUE(vol.CreateFrom(cfg).ok());
}

TEST_F(VolumeImplTest, CreateFromUnsupportedEngine) {
  auto cfg = makeConfig("redis://localhost:6379/0", tmpdir_);
  VolumeImpl vol;
  Status st = vol.CreateFrom(cfg);
  EXPECT_FALSE(st.ok());
}

TEST_F(VolumeImplTest, CreateFromNoConfigPathSkipsWrite) {
  auto cfg = makeConfig("memory://local", "");  // empty → skip WriteToFile
  VolumeImpl vol;
  EXPECT_TRUE(vol.CreateFrom(cfg).ok());
}

TEST_F(VolumeImplTest, CreateFromWriteToFileFailure) {
  auto cfg = makeConfig("memory://local", "/root/no-permission-dir");
  VolumeImpl vol;
  Status st = vol.CreateFrom(cfg);
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
  auto cfg = makeConfig("memory://local", "/tmp/nonexistent_vol_impl_dir");
  VolumeImpl vol;
  Status st = vol.LoadFrom(cfg);
  EXPECT_FALSE(st.ok());
}
