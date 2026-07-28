// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for CLI validators defined in src/config/Validator.cpp.

#include <gtest/gtest.h>

#include <string>

#include "config/Validator.hpp"
#include "storage/StorageRegistry.hpp"

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

TEST(ValidateMetaUrlTest, UnknownValue) {
  EXPECT_FALSE(ValidateMetaUrl("redis://localhost").empty());
  EXPECT_FALSE(ValidateMetaUrl("postgres://host/db").empty());
  EXPECT_FALSE(ValidateMetaUrl("not-a-valid-url").empty());
}

TEST(ValidateMetaUrlTest, ErrorMessageContainsScheme) {
  std::string err = ValidateMetaUrl("redis://localhost");
  EXPECT_NE(err.find("redis"), std::string::npos);
  EXPECT_NE(err.find("Supported"), std::string::npos)
      << "Error message should mention 'Supported': " << err;
}