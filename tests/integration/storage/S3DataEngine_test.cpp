// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for S3DataEngine — direct Put/Get/Head/Delete testing
// against a real S3-compatible endpoint.
//
// Requires environment variables:
//   SWORDFS_TEST_S3_ENDPOINT  e.g. "http://localhost:4566" (localstack)
//   SWORDFS_TEST_S3_REGION    e.g. "us-east-1"
//   SWORDFS_TEST_S3_BUCKET    bucket name (must already exist)
//   SWORDFS_TEST_S3_PREFIX    optional, defaults to "swordfs-test"

#ifdef SWORDFS_ENABLE_S3

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "storage/IDataEngine.hpp"
#include "storage/s3/S3DataEngine.hpp"
#include "utils/Status.hpp"

using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::storage::S3Config;
using swordfs::storage::S3DataEngine;
using swordfs::utils::Status;

namespace {

// Read S3 config from environment.  Returns true if all required vars are
// set; otherwise the test is skipped.
bool LoadConfig(S3Config* cfg) {
  const char* endpoint = std::getenv("SWORDFS_TEST_S3_ENDPOINT");
  const char* region = std::getenv("SWORDFS_TEST_S3_REGION");
  const char* bucket = std::getenv("SWORDFS_TEST_S3_BUCKET");

  if (!endpoint || !region || !bucket) return false;

  cfg->endpoint = endpoint;
  cfg->region = region;
  cfg->bucket = bucket;

  const char* prefix = std::getenv("SWORDFS_TEST_S3_PREFIX");
  cfg->prefix = prefix ? prefix : "swordfs-test";

  return true;
}

// Generate a unique test key so parallel test runs don't collide.
std::string TestKey(std::string_view name) {
  static std::atomic<int> counter{0};
  return std::string(name) + "-" +
         std::to_string(
             std::hash<std::thread::id>{}(std::this_thread::get_id())) +
         "-" + std::to_string(counter.fetch_add(1));
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────
// S3DataEngineTest fixture
// ────────────────────────────────────────────────────────────────────────

class S3DataEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    S3Config cfg;
    if (!LoadConfig(&cfg)) {
      GTEST_SKIP() << "S3 env vars not set: SWORDFS_TEST_S3_ENDPOINT, "
                      "SWORDFS_TEST_S3_REGION, SWORDFS_TEST_S3_BUCKET";
    }
    cfg_ = cfg;
    engine_ = std::make_unique<S3DataEngine>(cfg_);
  }

  void TearDown() override {
    // Clean up keys created during the test.
    for (const auto& key : created_keys_) {
      engine_->Delete(key);
    }
  }

  /// Write data and track the key for cleanup.
  Status Put(std::string_view key, std::string_view data) {
    created_keys_.push_back(std::string(key));
    return engine_->Put(key, data);
  }

  S3Config cfg_;
  std::unique_ptr<S3DataEngine> engine_;
  std::vector<std::string> created_keys_;
};

// ────────────────────────────────────────────────────────────────────────
// Limits
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, LimitsDefaults) {
  DataEngineLimits limits = engine_->Limits();
  EXPECT_EQ(limits.max_chunk_size, 64u * 1024 * 1024);
  EXPECT_FALSE(limits.supports_multipart);
  EXPECT_FALSE(limits.supports_overwrite);
}

// ────────────────────────────────────────────────────────────────────────
// ObjectKey
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, ObjectKeyWithPrefix) {
  EXPECT_EQ(engine_->ObjectKey("chunks/42/0"),
            cfg_.prefix + "/chunks/42/0");
}

TEST_F(S3DataEngineTest, ObjectKeyEmptyPrefix) {
  S3Config cfg = cfg_;
  cfg.prefix = "";
  S3DataEngine eng(cfg);
  EXPECT_EQ(eng.ObjectKey("chunks/1/0"), "chunks/1/0");
}

// ────────────────────────────────────────────────────────────────────────
// Put + Get round-trip
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, PutAndGetSmallData) {
  std::string key = TestKey("put-get-small");
  std::string data = "hello swordfs s3 data engine";

  Status s = Put(key, data);
  ASSERT_TRUE(s.ok()) << s.message();

  std::string result;
  s = engine_->Get(key, &result);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(result, data);
}

TEST_F(S3DataEngineTest, PutAndGetEmptyData) {
  std::string key = TestKey("put-get-empty");

  Status s = Put(key, "");
  ASSERT_TRUE(s.ok()) << s.message();

  std::string result;
  s = engine_->Get(key, &result);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_TRUE(result.empty());
}

TEST_F(S3DataEngineTest, PutAndGetLargeData) {
  std::string key = TestKey("put-get-large");
  // 1 MiB of deterministic pseudo-data.
  std::string data(1024 * 1024, '\0');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>(i % 251 + 1);
  }

  Status s = Put(key, data);
  ASSERT_TRUE(s.ok()) << s.message();

  std::string result;
  s = engine_->Get(key, &result);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(result, data);
}

TEST_F(S3DataEngineTest, PutAndGetBinaryData) {
  std::string key = TestKey("put-get-binary");
  std::string data(256, '\0');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>(i);
  }

  Status s = Put(key, data);
  ASSERT_TRUE(s.ok()) << s.message();

  std::string result;
  s = engine_->Get(key, &result);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(result, data);
}

// ────────────────────────────────────────────────────────────────────────
// Get with range (offset / size)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, GetWithOffset) {
  std::string key = TestKey("get-offset");
  std::string data = "0123456789ABCDEF";

  ASSERT_TRUE(Put(key, data).ok());

  std::string result;
  Status s = engine_->Get(key, &result, 4, 0);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(result, "456789ABCDEF");
}

TEST_F(S3DataEngineTest, GetWithOffsetAndSize) {
  std::string key = TestKey("get-offset-size");
  std::string data = "0123456789ABCDEF";

  ASSERT_TRUE(Put(key, data).ok());

  std::string result;
  Status s = engine_->Get(key, &result, 4, 6);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(result, "456789");
}

TEST_F(S3DataEngineTest, GetZeroSize) {
  std::string key = TestKey("get-zero-size");
  ASSERT_TRUE(Put(key, "data").ok());

  std::string result;
  Status s = engine_->Get(key, &result, 0, 0);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(result, "data");  // size=0 means "to end"
}

// ────────────────────────────────────────────────────────────────────────
// Get non-existent key
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, GetNotFound) {
  std::string key = TestKey("nonexistent");
  // Ensure the key doesn't exist.
  engine_->Delete(key);

  std::string result;
  Status s = engine_->Get(key, &result);
  EXPECT_TRUE(s.IsNotFound())
      << "Expected NotFound, got code=" << static_cast<int>(s.code())
      << " msg=" << s.message();
  EXPECT_TRUE(result.empty());
}

// ────────────────────────────────────────────────────────────────────────
// Head
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, HeadExisting) {
  std::string key = TestKey("head-existing");
  std::string data = "check my size please";

  ASSERT_TRUE(Put(key, data).ok());

  size_t size = 0;
  EXPECT_TRUE(engine_->Head(key, &size));
  EXPECT_EQ(size, data.size());
}

TEST_F(S3DataEngineTest, HeadNonExistent) {
  std::string key = TestKey("head-missing");
  engine_->Delete(key);

  EXPECT_FALSE(engine_->Head(key, nullptr));
}

TEST_F(S3DataEngineTest, HeadNullSize) {
  std::string key = TestKey("head-null-size");
  ASSERT_TRUE(Put(key, "something").ok());

  // Should not crash when size pointer is null.
  EXPECT_TRUE(engine_->Head(key, nullptr));
}

// ────────────────────────────────────────────────────────────────────────
// Delete
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, DeleteExisting) {
  std::string key = TestKey("delete-existing");
  ASSERT_TRUE(Put(key, "delete me").ok());

  Status s = engine_->Delete(key);
  EXPECT_TRUE(s.ok()) << s.message();

  // Verify it's gone.
  std::string result;
  s = engine_->Get(key, &result);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(S3DataEngineTest, DeleteNonExistent) {
  std::string key = TestKey("delete-missing");
  engine_->Delete(key);  // Ensure clean state.

  // Deleting a non-existent key should succeed (idempotent).
  Status s = engine_->Delete(key);
  EXPECT_TRUE(s.ok()) << s.message();
}

// ────────────────────────────────────────────────────────────────────────
// Overwrite (re-Put same key)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, PutOverwritesExistingKey) {
  std::string key = TestKey("overwrite");
  ASSERT_TRUE(Put(key, "version 1").ok());
  ASSERT_TRUE(engine_->Put(key, "version 2").ok());

  std::string result;
  ASSERT_TRUE(engine_->Get(key, &result).ok());
  EXPECT_EQ(result, "version 2");
}

// ────────────────────────────────────────────────────────────────────────
// Multiple keys
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, MultipleIndependentKeys) {
  std::string k1 = TestKey("multi-1");
  std::string k2 = TestKey("multi-2");
  std::string k3 = TestKey("multi-3");

  ASSERT_TRUE(Put(k1, "aaa").ok());
  ASSERT_TRUE(Put(k2, "bbb").ok());
  ASSERT_TRUE(Put(k3, "ccc").ok());

  std::string r;
  ASSERT_TRUE(engine_->Get(k1, &r).ok());
  EXPECT_EQ(r, "aaa");
  ASSERT_TRUE(engine_->Get(k2, &r).ok());
  EXPECT_EQ(r, "bbb");
  ASSERT_TRUE(engine_->Get(k3, &r).ok());
  EXPECT_EQ(r, "ccc");
}

// ────────────────────────────────────────────────────────────────────────
// Engine factory / Limits consistency
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3DataEngineTest, ChunkKeyDeterministic) {
  // Verify that chunk key generation is deterministic — two keys
  // derived from the same inode + offset are identical.
  // This is critical for the VFS layer's ability to locate chunks
  // without metadata lookups.
  std::string k1 = engine_->ObjectKey("chunks/42/0");
  std::string k2 = engine_->ObjectKey("chunks/42/0");
  EXPECT_EQ(k1, k2);
}

}  // namespace

#else  // !SWORDFS_ENABLE_S3

// When S3 is not enabled, provide a single dummy test so the test
// binary links but reports that S3 tests were skipped.
#include <gtest/gtest.h>
TEST(S3DataEngineTest, DISABLED_S3NotEnabled) {
  GTEST_SKIP() << "S3 support not compiled (ENABLE_S3=OFF)";
}

#endif  // SWORDFS_ENABLE_S3
