// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for ConfigCenter CLI parameter combinations.

#include <gtest/gtest.h>

#include <CLI/CLI.hpp>
#include <string>
#include <vector>

#include "config/ConfigCenter.hpp"

namespace {

// Helper: parse a vector of argument strings and return the error message.
// Returns empty string on success (no error).
std::string ParseOptions(std::vector<std::string> args) {
  CLI::App app{"SwordFS test"};
  app.allow_extras(false);

  auto& cfg = swordfs::config::ConfigCenter::Instance();
  cfg.ResetForTesting();
  cfg.ConfigureOptions(app);

  // Convert to argc/argv as CLI11's vector-based parse can behave
  // differently from argc/argv-based parse in some versions.
  std::vector<const char*> argv;
  argv.reserve(args.size());
  for (const auto& a : args) argv.push_back(a.c_str());
  int argc = static_cast<int>(argv.size());

  try {
    app.parse(argc, argv.data());
  } catch (const CLI::ParseError& e) {
    return e.what();
  }
  return {};
}

}  // namespace

// ================================================================
// format — valid parameter combinations
// ================================================================

TEST(FormatParamsTest, MinimalMemoryFormat) {
  // Bare minimum for format with in-memory metadata engine.
  std::vector<std::string> args = {
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--bucket",
      "s3://mybucket.s3.amazonaws.com/chunks",
      "--volume-config-path",
      "/tmp/cfg",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

TEST(FormatParamsTest, MemoryFormatWithRegion) {
  std::vector<std::string> args = {
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--bucket",
      "s3://mybucket.s3.amazonaws.com/chunks",
      "--volume-config-path",
      "/tmp/cfg",
      "--storage-region",
      "us-east-1",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

// ================================================================
// mount — valid parameter combinations
// ================================================================

TEST(MountParamsTest, MinimalMemoryMount) {
  // Bare minimum for mount with in-memory metadata engine.
  std::vector<std::string> args = {
      "swordfs",
      "mount",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--volume-config-path",
      "/tmp/cfg",
      "/mnt/point",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

TEST(MountParamsTest, MountWithFuseOpts) {
  std::vector<std::string> args = {
      "swordfs",
      "mount",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--volume-config-path",
      "/tmp/cfg",
      "-o",
      "allow_other,ro",
      "/mnt/point",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

// ================================================================
// format — invalid parameter combinations
// ================================================================

TEST(FormatParamsTest, MissingVolumeConfigPathWithMemoryMeta) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--bucket",
      "s3://mybucket.s3.amazonaws.com/chunks",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("volume-config-path"), std::string::npos) << err;
}

TEST(FormatParamsTest, VolumeConfigPathWithoutMemoryMeta) {
  // --volume-config-path without --meta.
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--bucket",
      "s3://mybucket.s3.amazonaws.com/chunks",
      "--volume-config-path",
      "/tmp/cfg",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("--meta"), std::string::npos) << err;
}

TEST(FormatParamsTest, MissingBucket) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--volume-config-path",
      "/tmp/cfg",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("--bucket"), std::string::npos) << err;
}

TEST(FormatParamsTest, MissingVolume) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--meta",
      "memory://local",
      "--bucket",
      "s3://mybucket.s3.amazonaws.com/chunks",
      "--volume-config-path",
      "/tmp/cfg",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("--volume"), std::string::npos) << err;
}

TEST(FormatParamsTest, InvalidBucketScheme) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--bucket",
      "https://example.com/bucket",
      "--volume-config-path",
      "/tmp/cfg",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("scheme"), std::string::npos) << err;
}

TEST(FormatParamsTest, BucketWithoutScheme) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--bucket",
      "no-scheme-bucket",
      "--volume-config-path",
      "/tmp/cfg",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("scheme"), std::string::npos) << err;
}

TEST(FormatParamsTest, UnknownFlag) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--bucket",
      "s3://mybucket.s3.amazonaws.com/chunks",
      "--volume-config-path",
      "/tmp/cfg",
      "--storage",
      "s3",
  });
  EXPECT_FALSE(err.empty());
  // CLI11 uses "not a recognized" or similar for unknown flags.
  EXPECT_NE(err.find("--storage"), std::string::npos) << err;
}

// ================================================================
// mount — invalid parameter combinations
// ================================================================

TEST(MountParamsTest, MissingVolumeConfigPathWithMemoryMeta) {
  auto err = ParseOptions({
      "swordfs",
      "mount",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "/mnt/point",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("volume-config-path"), std::string::npos) << err;
}

TEST(MountParamsTest, VolumeConfigPathWithoutMemoryMeta) {
  auto err = ParseOptions({
      "swordfs",
      "mount",
      "--volume",
      "myvol",
      "--volume-config-path",
      "/tmp/cfg",
      "/mnt/point",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("--meta"), std::string::npos) << err;
}

TEST(MountParamsTest, MissingMountpoint) {
  auto err = ParseOptions({
      "swordfs",
      "mount",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
      "--volume-config-path",
      "/tmp/cfg",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("mountpoint"), std::string::npos) << err;
}

TEST(MountParamsTest, MissingVolume) {
  auto err = ParseOptions({
      "swordfs",
      "mount",
      "--meta",
      "memory://local",
      "--volume-config-path",
      "/tmp/cfg",
      "/mnt/point",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("--volume"), std::string::npos) << err;
}
