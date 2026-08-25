// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <folly/io/IOBuf.h>

#include <memory>

#include "storage/DataEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"

namespace {

class TestDataEngine : public swordfs::storage::IDataEngine {
 public:
  swordfs::utils::Status Initialize() override { return swordfs::utils::Status::OK(); }
  swordfs::storage::DataEngineLimits Limits() const override { return {}; }

  bool Head(std::string_view, size_t *) override { return false; }

  swordfs::utils::Status Put(std::string_view,
                             std::unique_ptr<folly::IOBuf>) override {
    return swordfs::utils::Status::OK();
  }

  swordfs::utils::Status Get(std::string_view, size_t, size_t,
                             folly::IOBuf *) override {
    return swordfs::utils::Status::OK();
  }

  swordfs::utils::Status Delete(std::string_view) override {
    return swordfs::utils::Status::OK();
  }
};

swordfs::utils::Status CreateTestEngine(std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  *out = std::make_unique<TestDataEngine>();
  return swordfs::utils::Status::OK();
}

}  // namespace

TEST(DataEngineRegistryTest, RegisteredEngineIsAvailableAndCanBeCreated) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  registry.Register("test", CreateTestEngine);

  EXPECT_TRUE(registry.Available("test"));

  std::unique_ptr<swordfs::storage::IDataEngine> engine;
  auto status = registry.Create("test", &engine);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_NE(engine, nullptr);
}

TEST(DataEngineRegistryTest, UnknownEngineIsNotSupported) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  EXPECT_FALSE(registry.Available("does-not-exist"));

  std::unique_ptr<swordfs::storage::IDataEngine> engine;
  auto status = registry.Create("does-not-exist", &engine);
  EXPECT_TRUE(status.IsNotSupported());
  EXPECT_EQ(engine, nullptr);
}

TEST(DataEngineRegistryTest, NullOutputIsRejected) {
  auto &registry = swordfs::storage::DataEngineRegistry::Instance();
  auto status = registry.Create("test", nullptr);
  EXPECT_EQ(status.code(), swordfs::utils::Status::kInvalidArgument);
}
