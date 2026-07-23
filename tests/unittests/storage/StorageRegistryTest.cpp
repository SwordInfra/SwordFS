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

// A minimal mock engine for testing the registry.
class MockEngine : public IDataEngine {
 public:
  explicit MockEngine(std::string name) : name_(std::move(name)) {}

  DataEngineLimits Limits() const override { return DataEngineLimits{}; }
  bool Head(std::string_view /*key*/, size_t* size) override {
    if (size) *size = 0;
    return false;
  }
  Status Put(std::string_view /*key*/, std::string_view /*data*/) override {
    return Status::OK();
  }
  Status Get(std::string_view /*key*/, std::string* out, size_t, size_t) override {
    out->clear();
    return Status::OK();
  }
  Status Delete(std::string_view /*key*/) override { return Status::OK(); }

  std::string name() const { return name_; }

 private:
  std::string name_;
};

std::unique_ptr<IDataEngine> MakeMock() {
  return std::make_unique<MockEngine>("mock");
}

}  // namespace

// ════════════════════════════════════════════════════════════════════
// StorageRegistryTest
// ════════════════════════════════════════════════════════════════════

class StorageRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Register a test backend so every test starts clean.
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
  auto* mock = dynamic_cast<MockEngine*>(engine.get());
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->name(), "mock");
}

TEST_F(StorageRegistryTest, CreateReturnsNullForUnknown) {
  auto engine = StorageRegistry::Instance().Create("unknown");
  EXPECT_EQ(engine, nullptr);
}

TEST_F(StorageRegistryTest, RegisterOverwritesExisting) {
  bool called = false;
  auto factory = [&]() -> std::unique_ptr<IDataEngine> {
    called = true;
    return std::make_unique<MockEngine>("overwritten");
  };
  StorageRegistry::Instance().Register("mock", factory);

  auto engine = StorageRegistry::Instance().Create("mock");
  ASSERT_NE(engine, nullptr);
  EXPECT_TRUE(called);
}
