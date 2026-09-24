// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "storage/s3/S3DataEngine.hpp"

namespace swordfs::storage {
namespace {

TEST(S3DataEngineTest, RejectsInvalidBucketLocations) {
  S3DataEngine wrong_scheme(DataEngineOptions{.location = "memory://local"});
  EXPECT_EQ(wrong_scheme.Initialize().ToErrno(), EINVAL);

  S3DataEngine missing_bucket(DataEngineOptions{.location = "s3://endpoint.example.com"});
  EXPECT_EQ(missing_bucket.Initialize().ToErrno(), EINVAL);
}

TEST(S3DataEngineTest, ObjectKeyPreservesKeyWithoutPrefix) {
  S3DataEngine engine(DataEngineOptions{.location = "s3://endpoint.example.com/bucket"});

  EXPECT_EQ(engine.ObjectKey("chunk-42"), "chunk-42");
}

}  // namespace
}  // namespace swordfs::storage
