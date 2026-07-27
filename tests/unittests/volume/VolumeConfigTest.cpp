// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VolumeConfig.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "utils/Status.hpp"
#include "volume/VolumeConfig.hpp"

using swordfs::utils::Status;
using swordfs::volume::VolumeConfig;

namespace {

// Helper: create a temp directory and return its path.
std::string MakeTempDir() {
  char tmpl[] = "/tmp/swordfs_test_XXXXXX";
  const char* path = mkdtemp(tmpl);
  EXPECT_NE(path, nullptr);
  return std::string(path);
}

// Helper: remove a directory and its contents.
void RemoveDir(const std::string& path) {
  if (!path.empty() && path.rfind("/tmp/", 0) == 0) {
    std::string cmd = "rm -rf " + path;
    std::system(cmd.c_str());
  }
}

}  // namespace

// ────────────────────────────────────────────────────────────────
// ToJson / FromJson round-trip
// ────────────────────────────────────────────────────────────────

TEST(VolumeConfigTest, ToJsonAndFromJsonRoundTrip) {
  VolumeConfig original;
  original.name = "test-vol";
  original.meta_url = "memory://local";
  original.storage = "s3";
  original.bucket = "s3://endpoint/mybucket/prefix";
  original.region = "us-east-1";

  std::string json = original.ToJson();
  EXPECT_NE(json.find("test-vol"), std::string::npos);
  EXPECT_NE(json.find("us-east-1"), std::string::npos);

  VolumeConfig parsed;
  Status st = parsed.FromJson(json);
  ASSERT_TRUE(st.ok()) << st.message();

  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.meta_url, original.meta_url);
  EXPECT_EQ(parsed.storage, original.storage);
  EXPECT_EQ(parsed.bucket, original.bucket);
  EXPECT_EQ(parsed.region, original.region);
}

TEST(VolumeConfigTest, ToJsonAndFromJsonRoundTripRegionAuto) {
  VolumeConfig original;
  original.name = "v2";
  original.meta_url = "memory://local";
  original.storage = "s3";
  original.bucket = "s3://host/bucket";
  original.region = "auto";

  VolumeConfig parsed;
  Status st = parsed.FromJson(original.ToJson());
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(parsed.region, "auto");
}

// ────────────────────────────────────────────────────────────────
// FromJson — error cases
// ────────────────────────────────────────────────────────────────

TEST(VolumeConfigTest, FromJsonInvalidJson) {
  VolumeConfig v;
  Status st = v.FromJson("not json");
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  EXPECT_NE(st.message().find("invalid JSON"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonRootNotObject) {
  VolumeConfig v;
  Status st = v.FromJson("[1, 2, 3]");
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  EXPECT_NE(st.message().find("not an object"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonMissingName) {
  VolumeConfig v;
  Status st = v.FromJson(R"({"meta":"m","storage":"s","bucket":"b","region":"r"})");
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("name"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonMissingMeta) {
  VolumeConfig v;
  Status st = v.FromJson(R"({"name":"n","storage":"s","bucket":"b","region":"r"})");
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("meta"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonMissingStorage) {
  VolumeConfig v;
  Status st = v.FromJson(R"({"name":"n","meta":"m","bucket":"b","region":"r"})");
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("storage"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonMissingBucket) {
  VolumeConfig v;
  Status st = v.FromJson(R"({"name":"n","meta":"m","storage":"s","region":"r"})");
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("bucket"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonMissingRegion) {
  VolumeConfig v;
  Status st = v.FromJson(R"({"name":"n","meta":"m","storage":"s","bucket":"b"})");
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("region"), std::string::npos);
}

TEST(VolumeConfigTest, FromJsonWrongType) {
  VolumeConfig v;
  Status st = v.FromJson(R"({"name":123,"meta":"m","storage":"s","bucket":"b","region":"r"})");
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("name"), std::string::npos);
}

// ────────────────────────────────────────────────────────────────
// WriteToFile / ReadFromFile round-trip
// ────────────────────────────────────────────────────────────────

TEST(VolumeConfigTest, WriteAndReadRoundTrip) {
  std::string dir = MakeTempDir();

  VolumeConfig original;
  original.name = "persist-test";
  original.meta_url = "memory://local";
  original.storage = "s3";
  original.bucket = "s3://myhost/mybucket";
  original.region = "auto";

  Status st = original.WriteToFile(dir);
  ASSERT_TRUE(st.ok()) << st.message();

  // Verify the file exists.
  std::ifstream ifs(dir + "/volume.json");
  ASSERT_TRUE(ifs.good());

  VolumeConfig restored;
  st = restored.ReadFromFile(dir);
  ASSERT_TRUE(st.ok()) << st.message();

  EXPECT_EQ(restored.name, original.name);
  EXPECT_EQ(restored.meta_url, original.meta_url);
  EXPECT_EQ(restored.storage, original.storage);
  EXPECT_EQ(restored.bucket, original.bucket);
  EXPECT_EQ(restored.region, original.region);

  RemoveDir(dir);
}

TEST(VolumeConfigTest, WriteToFileCreatesParentDir) {
  std::string dir = MakeTempDir();
  std::string subdir = dir + "/nested/path";

  VolumeConfig v;
  v.name = "nested-vol";
  v.meta_url = "memory://local";
  v.storage = "s3";
  v.bucket = "s3://h/b";
  v.region = "auto";

  Status st = v.WriteToFile(subdir);
  ASSERT_TRUE(st.ok()) << st.message();

  std::ifstream ifs(subdir + "/volume.json");
  ASSERT_TRUE(ifs.good());

  RemoveDir(dir);
}

TEST(VolumeConfigTest, ReadFromFileNotFound) {
  VolumeConfig v;
  Status st = v.ReadFromFile("/tmp/nonexistent_dir_xyz123");
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kNotFound);
  EXPECT_NE(st.message().find("not found"), std::string::npos);
}

TEST(VolumeConfigTest, WriteToFileNotADirectory) {
  // Use an existing regular file as the path.
  std::string dir = MakeTempDir();
  std::string file_path = dir + "/regular_file";
  {
    std::ofstream ofs(file_path);
    ofs << "hello";
  }

  VolumeConfig v;
  v.name = "n";
  v.meta_url = "m";
  v.storage = "s";
  v.bucket = "b";
  v.region = "r";

  Status st = v.WriteToFile(file_path);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  EXPECT_NE(st.message().find("not a directory"), std::string::npos);

  RemoveDir(dir);
}

// ────────────────────────────────────────────────────────────────
// MountHint
// ────────────────────────────────────────────────────────────────

TEST(VolumeConfigTest, MountHintContainsKeyFields) {
  VolumeConfig v;
  v.name = "myvol";
  v.meta_url = "memory://local";
  v.storage = "s3";
  v.bucket = "s3://h/b";
  v.region = "auto";

  std::string hint = v.MountHint();
  EXPECT_NE(hint.find("myvol"), std::string::npos);
  EXPECT_NE(hint.find("memory://local"), std::string::npos);
  EXPECT_NE(hint.find("mount"), std::string::npos);
}
