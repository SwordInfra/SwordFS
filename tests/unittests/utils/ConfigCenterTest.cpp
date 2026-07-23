// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "utils/ConfigCenter.hpp"

using swordfs::utils::ConfigCenter;

// ════════════════════════════════════════════════════════════════════
// ConfigCenterTest — storage config defaults
// ════════════════════════════════════════════════════════════════════

TEST(ConfigCenterTest, StorageBackendDefaultsToEmpty) {
  auto& cfg = ConfigCenter::Instance();
  // Default: no storage backend (memory-only mode).
  EXPECT_TRUE(cfg.storage_backend().empty());
}

TEST(ConfigCenterTest, S3DefaultsAreSet) {
  auto& cfg = ConfigCenter::Instance();
  EXPECT_EQ(cfg.s3_endpoint(), "https://s3.amazonaws.com");
  EXPECT_EQ(cfg.s3_region(), "us-east-1");
  EXPECT_EQ(cfg.s3_prefix(), "swordfs/chunks");
  EXPECT_TRUE(cfg.s3_bucket().empty());  // no default bucket
}
