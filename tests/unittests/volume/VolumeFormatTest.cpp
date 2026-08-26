// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for SwordFsVolume and the memory volume.json representation.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "metadata/mem/VolumeJson.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::SwordFsVolume;
using swordfs::metadata::mem::VolumeJson;
using swordfs::utils::Status;

namespace {
std::string MakeTempDir() {
  char tmpl[] = "/tmp/swordfs_test_XXXXXX";
  const char *path = mkdtemp(tmpl);
  EXPECT_NE(path, nullptr);
  return std::string(path);
}

void RemoveDir(const std::string &path) {
  if (!path.empty() && path.rfind("/tmp/", 0) == 0) {
    std::string cmd = "rm -rf " + path;
    std::system(cmd.c_str());
  }
}

SwordFsVolume MakeVolume() {
  SwordFsVolume volume;
  volume.name = "test-vol";
  volume.meta_url = "memory://local";
  volume.storage = "s3";
  volume.bucket = "s3://endpoint/mybucket/prefix";
  volume.region = "us-east-1";
  return volume;
}
}  // namespace

TEST(VolumeJsonTest, ToJsonAndFromJsonRoundTrip) {
  SwordFsVolume original = MakeVolume();
  std::string json = VolumeJson::Serialize(original);
  EXPECT_NE(json.find("test-vol"), std::string::npos);
  EXPECT_NE(json.find("us-east-1"), std::string::npos);

  SwordFsVolume parsed;
  Status st = VolumeJson::Parse(json, &parsed);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.meta_url, original.meta_url);
  EXPECT_EQ(parsed.storage, original.storage);
  EXPECT_EQ(parsed.bucket, original.bucket);
  EXPECT_EQ(parsed.region, original.region);
}

TEST(VolumeJsonTest, ToJsonAndFromJsonRoundTripRegionAuto) {
  SwordFsVolume original = MakeVolume();
  original.name = "v2";
  original.region = "auto";
  SwordFsVolume parsed;
  Status st = VolumeJson::Parse(VolumeJson::Serialize(original), &parsed);
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(parsed.region, "auto");
}

TEST(SwordFsVolumeTest, SerializeToAndParseFromRoundTrip) {
  SwordFsVolume original = MakeVolume();
  original.meta_url = "redis://localhost:6379/3";
  original.chunk_size = 128ULL * 1024 * 1024;

  std::string encoded = original.SerializeTo();
  ASSERT_FALSE(encoded.empty());
  SwordFsVolume parsed;
  Status st = parsed.ParseFrom(encoded);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.meta_url, original.meta_url);
  EXPECT_EQ(parsed.storage, original.storage);
  EXPECT_EQ(parsed.bucket, original.bucket);
  EXPECT_EQ(parsed.region, original.region);
  EXPECT_EQ(parsed.chunk_size, original.chunk_size);
}

TEST(SwordFsVolumeTest, ParseFromRejectsMalformedData) {
  SwordFsVolume v;
  Status st = v.ParseFrom("not volume metadata");
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kMalformed);
}

TEST(VolumeJsonTest, FromJsonInvalidJson) {
  SwordFsVolume v;
  Status st = VolumeJson::Parse("not json", &v);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  EXPECT_NE(st.message().find("invalid JSON"), std::string::npos);
}

TEST(VolumeJsonTest, FromJsonRootNotObject) {
  SwordFsVolume v;
  Status st = VolumeJson::Parse("[1, 2, 3]", &v);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  EXPECT_NE(st.message().find("not an object"), std::string::npos);
}

TEST(VolumeJsonTest, FromJsonMissingFields) {
  SwordFsVolume v;
  Status st = VolumeJson::Parse(R"({"meta":"m","storage":"s","bucket":"b","region":"r"})", &v);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("name"), std::string::npos);
}

TEST(VolumeJsonTest, FromJsonWrongType) {
  SwordFsVolume v;
  Status st = VolumeJson::Parse(R"({"name":123,"meta":"m","storage":"s","bucket":"b","region":"r"})", &v);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("name"), std::string::npos);
}

TEST(VolumeJsonTest, WriteAndReadRoundTrip) {
  std::string dir = MakeTempDir();
  SwordFsVolume original = MakeVolume();
  Status st = VolumeJson::Write(original, dir);
  ASSERT_TRUE(st.ok()) << st.message();

  std::ifstream ifs(dir + "/volume.json");
  ASSERT_TRUE(ifs.good());

  SwordFsVolume restored;
  st = VolumeJson::Read(dir, &restored);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(restored.name, original.name);
  EXPECT_EQ(restored.meta_url, original.meta_url);
  EXPECT_EQ(restored.storage, original.storage);
  EXPECT_EQ(restored.bucket, original.bucket);
  EXPECT_EQ(restored.region, original.region);
  RemoveDir(dir);
}

TEST(VolumeJsonTest, WriteCreatesParentDir) {
  std::string dir = MakeTempDir();
  std::string subdir = dir + "/nested/path";
  SwordFsVolume v = MakeVolume();
  Status st = VolumeJson::Write(v, subdir);
  ASSERT_TRUE(st.ok()) << st.message();
  std::ifstream ifs(subdir + "/volume.json");
  ASSERT_TRUE(ifs.good());
  RemoveDir(dir);
}

TEST(VolumeJsonTest, ReadNotFound) {
  SwordFsVolume v;
  Status st = VolumeJson::Read("/tmp/nonexistent_dir_xyz123", &v);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kNotFound);
}

TEST(VolumeJsonTest, WriteNotADirectory) {
  std::string dir = MakeTempDir();
  std::string file_path = dir + "/regular_file";
  {
    std::ofstream ofs(file_path);
    ofs << "hello";
  }
  SwordFsVolume v = MakeVolume();
  Status st = VolumeJson::Write(v, file_path);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  RemoveDir(dir);
}

TEST(VolumeJsonTest, Exists) {
  std::string dir = MakeTempDir();
  EXPECT_FALSE(VolumeJson::Exists(dir));
  ASSERT_TRUE(VolumeJson::Write(MakeVolume(), dir).ok());
  EXPECT_TRUE(VolumeJson::Exists(dir));
  RemoveDir(dir);
}
