// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "storage/IDataEngine.hpp"
#include "storage/StorageRegistry.hpp"

using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::storage::StorageRegistry;
using swordfs::utils::Status;

namespace {

class MockEngine : public IDataEngine {
 public:
  explicit MockEngine(std::string name) : name_(std::move(name)) {}
  DataEngineLimits Limits() const override { return DataEngineLimits{}; }
  bool Head(std::string_view, size_t* size) override {
    if (size) *size = 0;
    return false;
  }
  Status Put(std::string_view, std::string_view) override { return Status::OK(); }
  Status Get(std::string_view, std::string* out, size_t, size_t) override {
    out->clear();
    return Status::OK();
  }
  Status Delete(std::string_view) override { return Status::OK(); }
  std::string name() const { return name_; }
 private:
  std::string name_;
};

std::unique_ptr<IDataEngine> MakeMock() {
  return std::make_unique<MockEngine>("mock");
}

}  // namespace

class StorageRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    StorageRegistry::Instance().Register("mock", &MakeMock);
  }
};

TEST_F(StorageRegistryTest, AvailableReturnsTrueForRegistered) {
  EXPECT_TRUE(StorageRegistry::Instance().Available("mock"));
}

TEST_F(StorageRegistryTest, AvailableReturnsFalseForUnknown) {
  EXPECT_FALSE(StorageRegistry::Instance().Available("nonexistent"));
}

TEST_F(StorageRegistryTest, CreateReturnsEngineForRegistered) {
  auto engine = StorageRegistry::Instance().Create("mock");
  ASSERT_NE(engine, nullptr);
}

TEST_F(StorageRegistryTest, CreateReturnsNullForUnknown) {
  EXPECT_EQ(StorageRegistry::Instance().Create("unknown"), nullptr);
}

TEST_F(StorageRegistryTest, RegisterOverwritesExisting) {
  bool called = false;
  StorageRegistry::Instance().Register("mock", [&] {
    called = true;
    return std::make_unique<MockEngine>("overwritten");
  });
  auto engine = StorageRegistry::Instance().Create("mock");
  ASSERT_NE(engine, nullptr);
  EXPECT_TRUE(called);
}
