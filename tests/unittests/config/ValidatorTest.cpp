// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for CLI validators defined in src/config/Validator.cpp.

#include <gtest/gtest.h>

#include <string>

#include "config/Validator.hpp"

using swordfs::config::ValidateBucketUrl;
using swordfs::config::ValidateMetaUrl;

// ================================================================
// ValidateMetaUrl
// ================================================================

TEST(ValidateMetaUrlTest, ValidMemoryUrl) {
  // "memory://local" is the only supported scheme.
  EXPECT_TRUE(ValidateMetaUrl("memory://local").empty());
}

TEST(ValidateMetaUrlTest, ValidMemoryUrlWithPath) {
  EXPECT_FALSE(ValidateMetaUrl("memory://local/path/to/data").empty());
}

TEST(ValidateMetaUrlTest, EmptyString) {
  EXPECT_FALSE(ValidateMetaUrl("").empty());
}

TEST(ValidateMetaUrlTest, ValidRedisUrl) {
  EXPECT_TRUE(ValidateMetaUrl("redis://localhost").empty());
  EXPECT_TRUE(ValidateMetaUrl("redis://user:secret@localhost:6380/3").empty());
}

TEST(ValidateMetaUrlTest, UnknownValue) {
  EXPECT_FALSE(ValidateMetaUrl("redis://localhost:0").empty());
  const auto unsupported = ValidateMetaUrl("postgres://host/db");
  EXPECT_NE(unsupported.find("Unsupported metadata engine 'postgres'"), std::string::npos);

  const auto err = ValidateMetaUrl("not-a-valid-url");
  EXPECT_NE(err.find("Unsupported metadata engine 'not-a-valid-url'"), std::string::npos);
}

TEST(ValidateMetaUrlTest, InvalidRedisUrlHasHelpfulError) {
  std::string err = ValidateMetaUrl("redis://localhost:0");
  EXPECT_NE(err.find("invalid port"), std::string::npos) << "Error should describe the invalid Redis URL: " << err;
}

// ================================================================
// ValidateBucketUrl
// ================================================================

TEST(ValidateBucketUrlTest, ValidBucketUrl) {
  EXPECT_TRUE(ValidateBucketUrl("s3://endpoint.example.com/my-bucket").empty());
}

TEST(ValidateBucketUrlTest, ValidBucketUrlWithPrefix) {
  EXPECT_TRUE(ValidateBucketUrl("s3://endpoint.example.com/bucket/prefix").empty());
}

TEST(ValidateBucketUrlTest, MissingBucketName) {
  std::string err = ValidateBucketUrl("s3://endpoint.example.com");
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("missing bucket name"), std::string::npos)
      << "Error should mention 'missing bucket name': " << err;
}

TEST(ValidateBucketUrlTest, EmptyString) {
  EXPECT_FALSE(ValidateBucketUrl("").empty());
}

TEST(ValidateBucketUrlTest, NoScheme) {
  EXPECT_FALSE(ValidateBucketUrl("not-a-url").empty());
}

TEST(ValidateBucketUrlTest, UnsupportedScheme) {
  EXPECT_FALSE(ValidateBucketUrl("ftp://host/bucket").empty());
}