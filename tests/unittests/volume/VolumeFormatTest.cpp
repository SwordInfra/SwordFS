// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for SwordFsVolume and memory volume file persistence.

#include <gtest/gtest.h>

#include <string>
#include <unistd.h>

#include "metadata/mem/VolumeFile.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::SwordFsVolume;
using swordfs::utils::Status;

namespace {
bool ConfigRootWritable() {
  return access("/etc/swordfs", W_OK) == 0 || access("/etc", W_OK) == 0;
}

SwordFsVolume MakeVolume() {
  SwordFsVolume volume;
  volume.name = "test-vol-" + std::to_string(::getpid());
  volume.meta_url = "memory://local";
  volume.storage = "s3";
  volume.bucket = "s3://endpoint/mybucket/prefix";
  volume.region = "us-east-1";
  return volume;
}
}  // namespace

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

TEST(VolumeFileTest, WriteAndReadRoundTrip) {
  if (!ConfigRootWritable()) GTEST_SKIP() << "/etc/swordfs is not writable";
  SwordFsVolume original = MakeVolume();
  original.name = "volume-file-round-trip";
  swordfs::metadata::mem::VolumeFile file{original.name};
  Status st = file.Write(original);
  ASSERT_TRUE(st.ok()) << st.message();

  SwordFsVolume restored;
  st = file.Read(&restored);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(restored.name, original.name);
  EXPECT_EQ(restored.meta_url, original.meta_url);
  EXPECT_EQ(restored.storage, original.storage);
  EXPECT_EQ(restored.bucket, original.bucket);
  EXPECT_EQ(restored.region, original.region);
}

TEST(VolumeFileTest, WriteCreatesParentDir) {
  if (!ConfigRootWritable()) GTEST_SKIP() << "/etc/swordfs is not writable";
  SwordFsVolume v = MakeVolume();
  v.name = "volume-file-parent-dir";
  swordfs::metadata::mem::VolumeFile file{v.name};
  Status st = file.Write(v);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_TRUE(file.Exists());
}

TEST(VolumeFileTest, ReadNotFound) {
  SwordFsVolume v;
  swordfs::metadata::mem::VolumeFile file{"nonexistent-volume-file"};
  Status st = file.Read(&v);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), Status::kNotFound);
}

TEST(VolumeFileTest, Exists) {
  if (!ConfigRootWritable()) GTEST_SKIP() << "/etc/swordfs is not writable";
  SwordFsVolume v = MakeVolume();
  v.name = "volume-file-exists";
  swordfs::metadata::mem::VolumeFile file{v.name};
  EXPECT_FALSE(file.Exists());
  ASSERT_TRUE(file.Write(v).ok());
  EXPECT_TRUE(file.Exists());
}
