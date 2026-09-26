// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for ConfigCenter CLI parameter combinations.

#include <gtest/gtest.h>

#include <CLI/CLI.hpp>
#include <functional>
#include <string>
#include <vector>

#include "cmd/Mount.hpp"
#include "config/ConfigCenter.hpp"

namespace {

// Helper: parse a vector of argument strings and return the error message.
// Returns empty string on success (no error).
std::string ParseOptions(std::vector<std::string> args,
                         const std::function<void(const swordfs::config::ConfigCenter &)> &inspect = {}) {
  CLI::App app{"SwordFS test"};
  app.allow_extras(false);

  auto &cfg = swordfs::config::ConfigCenter::Instance();
  cfg.ConfigureOptions(app);

  // Convert to argc/argv as CLI11's vector-based parse can behave
  // differently from argc/argv-based parse in some versions.
  std::vector<const char *> argv;
  argv.reserve(args.size());
  for (const auto &a : args) {
    argv.push_back(a.c_str());
  }
  int argc = static_cast<int>(argv.size());

  try {
    app.parse(argc, argv.data());
  } catch (const CLI::ParseError &e) {
    return e.what();
  }
  if (inspect) {
    inspect(cfg);
  }
  return {};
}

}  // namespace

// ================================================================
// format — valid parameter combinations
// ================================================================

TEST(FormatParamsTest, MinimalMemoryFormat) {
  std::vector<std::string> args = {
      "swordfs", "format",         "--volume", "myvol",
      "--meta",  "memory://local", "--bucket", "s3://mybucket.s3.amazonaws.com/chunks",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

TEST(ConfigCenterTest, SelectsParsedSubcommandWithoutRetainingCliPointers) {
  CLI::App app{"SwordFS test"};
  auto &cfg = swordfs::config::ConfigCenter::Instance();
  cfg.ConfigureOptions(app);
  std::vector<std::string> args = {
      "swordfs", "format",         "--volume", "myvol",
      "--meta",  "memory://local", "--bucket", "s3://mybucket.s3.amazonaws.com/chunks",
  };
  std::vector<const char *> argv;
  argv.reserve(args.size());
  for (const auto &arg : args) {
    argv.push_back(arg.c_str());
  }
  app.parse(static_cast<int>(argv.size()), argv.data());

  const auto sub_command = cfg.SelectedSubCommand();
  ASSERT_TRUE(sub_command.has_value());
  EXPECT_EQ(sub_command->name, "format");
}

TEST(FormatParamsTest, MemoryFormatWithRegion) {
  std::vector<std::string> args = {
      "swordfs",          "format",         "--volume", "myvol",
      "--meta",           "memory://local", "--bucket", "s3://mybucket.s3.amazonaws.com/chunks",
      "--storage-region", "us-east-1",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

// ================================================================
// mount — valid parameter combinations
// ================================================================

TEST(MountParamsTest, MinimalMemoryMount) {
  std::vector<std::string> args = {
      "swordfs", "mount", "--volume", "myvol", "--meta", "memory://local", "/mnt/point",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

TEST(MountParamsTest, MountWithIndependentThreadCounts) {
  auto err = ParseOptions(
      {
          "swordfs",
          "mount",
          "--volume",
          "myvol",
          "--meta",
          "memory://local",
          "--storage-thread-count",
          "4",
          "--meta-thread-count",
          "7",
          "/mnt/point",
      },
      [](const swordfs::config::ConfigCenter &cfg) {
        EXPECT_EQ(cfg.storage_thread_count(), 4);
        EXPECT_EQ(cfg.meta_thread_count(), 7);
      });
  EXPECT_TRUE(err.empty()) << err;
}

TEST(MountParamsTest, MountWithFuseOpts) {
  std::vector<std::string> args = {
      "swordfs", "mount", "--volume", "myvol", "--meta", "memory://local", "-o", "allow_other,ro", "/mnt/point",
  };
  EXPECT_TRUE(ParseOptions(args).empty());
}

TEST(MountParamsTest, MandatoryDefaultPermissionsArePreservedWithUserOptions) {
  EXPECT_EQ(swordfs::cmd::detail::BuildFuseExtras(""), (std::vector<std::string>{"-o", "default_permissions"}));
  EXPECT_EQ(swordfs::cmd::detail::BuildFuseExtras("allow_other,ro"),
            (std::vector<std::string>{"-o", "default_permissions", "-o", "allow_other,ro"}));
}

TEST(MountParamsTest, NoAtimeRuntimeBehaviorUsesExactFuseOptionToken) {
  using swordfs::runtime::ImplicitAtimePolicy;

  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy("noatime"), ImplicitAtimePolicy::kDisabled);
  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy("allow_other,noatime,ro"), ImplicitAtimePolicy::kDisabled);
  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy(""), ImplicitAtimePolicy::kEnabled);
  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy("allow_other,ro"), ImplicitAtimePolicy::kEnabled);
  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy("xnoatime"), ImplicitAtimePolicy::kEnabled);
  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy("noatime_extra"), ImplicitAtimePolicy::kEnabled);
}

TEST(MountParamsTest, NoAtimeRuntimeBehaviorDoesNotRewriteFuseOptionForwarding) {
  EXPECT_EQ(swordfs::cmd::detail::ParseImplicitAtimePolicy("allow_other,noatime,ro"),
            swordfs::runtime::ImplicitAtimePolicy::kDisabled);
  EXPECT_EQ(swordfs::cmd::detail::BuildFuseExtras("allow_other,noatime,ro"),
            (std::vector<std::string>{"-o", "default_permissions", "-o", "allow_other,noatime,ro"}));
}

// ================================================================
// format — invalid parameter combinations
// ================================================================

TEST(FormatParamsTest, MissingBucket) {
  auto err = ParseOptions({
      "swordfs",
      "format",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
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
      "--not-a-real-option",
      "value",
  });
  EXPECT_FALSE(err.empty());
  // CLI11 uses "not a recognized" or similar for unknown flags.
  EXPECT_NE(err.find("--not-a-real-option"), std::string::npos) << err;
}

// ================================================================
// mount — invalid parameter combinations
// ================================================================

TEST(MountParamsTest, MissingMountpoint) {
  auto err = ParseOptions({
      "swordfs",
      "mount",
      "--volume",
      "myvol",
      "--meta",
      "memory://local",
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
      "/mnt/point",
  });
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("--volume"), std::string::npos) << err;
}
