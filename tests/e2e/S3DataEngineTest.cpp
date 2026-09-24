// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "storage/s3/S3DataEngine.hpp"
#include "tests/unittests/FiberTest.hpp"

namespace swordfs::storage {
namespace {

class S3DataEngineE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *bucket = std::getenv("SWORDFS_E2E_S3_BUCKET");
    ASSERT_NE(bucket, nullptr);
    ASSERT_NE(bucket[0], '\0');

    auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    prefix_ = "direct-s3-" + std::to_string(::getpid()) + "/" + test_info->name();

    std::string location(bucket);
    if (!location.empty() && location.back() != '/') {
      location += '/';
    }
    location += prefix_;

    DataEngineOptions options{.location = std::move(location), .worker_count = 2};
    if (const char *region = std::getenv("AWS_DEFAULT_REGION")) {
      options.region = region;
    }

    engine_ = std::make_unique<S3DataEngine>(std::move(options));
    const auto status = engine_->Initialize();
    ASSERT_TRUE(status.ok()) << status.message();
  }

  Status Put(std::string_view key, std::string_view value) {
    Status status;
    swordfs::test::RunInTestFiber([&] { status = engine_->Put(key, folly::IOBuf::copyBuffer(value)); });
    return status;
  }

  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) {
    Status status;
    swordfs::test::RunInTestFiber([&] { status = engine_->Get(key, offset, size, out); });
    return status;
  }

  Status Delete(std::string_view key) {
    Status status;
    swordfs::test::RunInTestFiber([&] { status = engine_->Delete(key); });
    return status;
  }

  std::unique_ptr<S3DataEngine> engine_;
  std::string prefix_;
};

TEST_F(S3DataEngineE2ETest, RoundTripsRangesMissingObjectsAndIdempotentDelete) {
  constexpr std::string_view kKey = "contract-object";
  constexpr std::string_view kPayload = "0123456789";

  EXPECT_EQ(engine_->ObjectKey(kKey), prefix_ + "/" + std::string(kKey));
  ASSERT_TRUE(Put(kKey, kPayload).ok());

  auto whole = folly::IOBuf::create(32);
  ASSERT_TRUE(Get(kKey, 0, 0, whole.get()).ok());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(whole->data()), whole->length()), kPayload);

  auto range = folly::IOBuf::create(4);
  ASSERT_TRUE(Get(kKey, 3, 4, range.get()).ok());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(range->data()), range->length()), "3456");

  auto suffix = folly::IOBuf::create(8);
  ASSERT_TRUE(Get(kKey, 6, 0, suffix.get()).ok());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(suffix->data()), suffix->length()), "6789");

  auto too_small = folly::IOBuf::create(3);
  EXPECT_EQ(Get(kKey, 0, 4, too_small.get()).ToErrno(), EINVAL);
  EXPECT_EQ(too_small->length(), 0);

  auto missing = folly::IOBuf::create(8);
  EXPECT_TRUE(Get("missing-object", 0, 0, missing.get()).IsNotFound());

  ASSERT_TRUE(Delete(kKey).ok());
  auto deleted = folly::IOBuf::create(8);
  EXPECT_TRUE(Get(kKey, 0, 0, deleted.get()).IsNotFound());
  EXPECT_TRUE(Delete(kKey).ok());
}

}  // namespace
}  // namespace swordfs::storage
