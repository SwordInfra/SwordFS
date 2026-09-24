// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <utility>

#include "storage/DataEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"

namespace {

class TestDataEngine : public swordfs::storage::IDataEngine {
 public:
  explicit TestDataEngine(swordfs::storage::DataEngineOptions options) : options_(std::move(options)) {
  }

  swordfs::utils::Status Initialize() override {
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return swordfs::utils::Status::OK();
  }

  swordfs::utils::Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return swordfs::utils::Status::OK();
  }

  swordfs::utils::Status Delete(std::string_view) override {
    return swordfs::utils::Status::OK();
  }

  const swordfs::storage::DataEngineOptions &options() const {
    return options_;
  }

 private:
  swordfs::storage::DataEngineOptions options_;
};

swordfs::utils::Status CreateTestEngine(const swordfs::storage::DataEngineOptions &options,
                                        std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  *out = std::make_unique<TestDataEngine>(options);
  return swordfs::utils::Status::OK();
}

}  // namespace

TEST(DataEngineRegistryTest, RegisteredEngineIsAvailableAndCanBeCreated) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  registry.Register("test", CreateTestEngine);

  EXPECT_TRUE(registry.Available("test"));

  swordfs::storage::DataEngineOptions options{
      .location = "s3://endpoint/bucket",
      .region = "test-region",
      .worker_count = 7,
  };
  std::unique_ptr<swordfs::storage::IDataEngine> engine;
  auto status = registry.CreateInstance("test", options, &engine);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_NE(engine, nullptr);
  const auto *test_engine = dynamic_cast<const TestDataEngine *>(engine.get());
  ASSERT_NE(test_engine, nullptr);
  EXPECT_EQ(test_engine->options().location, options.location);
  EXPECT_EQ(test_engine->options().region, options.region);
  EXPECT_EQ(test_engine->options().worker_count, options.worker_count);
}

TEST(DataEngineRegistryTest, UnknownEngineIsNotSupported) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  EXPECT_FALSE(registry.Available("does-not-exist"));

  std::unique_ptr<swordfs::storage::IDataEngine> engine;
  auto status = registry.CreateInstance("does-not-exist", {}, &engine);
  EXPECT_TRUE(status.ToErrno() == ENOSYS);
  EXPECT_EQ(engine, nullptr);
}

TEST(DataEngineRegistryTest, NullOutputIsRejected) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  auto status = registry.CreateInstance("test", {}, nullptr);
  EXPECT_EQ(status.ToErrno(), EINVAL);
}

TEST(DataEngineRegistryTest, S3RejectsInvalidConstructionLocation) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  for (const std::string location : {"not-a-url", "http://endpoint/bucket"}) {
    std::unique_ptr<swordfs::storage::IDataEngine> engine;
    auto status = registry.CreateInstance(
        "s3", swordfs::storage::DataEngineOptions{.location = location, .region = "test-region", .worker_count = 1},
        &engine);
    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_NE(engine, nullptr);
    status = engine->Initialize();
    EXPECT_EQ(status.ToErrno(), EINVAL) << location << ": " << status.message();
  }
}
