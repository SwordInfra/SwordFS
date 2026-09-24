// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for SwordFsVolume and memory volume file persistence.

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "metadata/mem/VolumeFile.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::SwordFsVolume;
using swordfs::utils::Status;

namespace {
class VolumeFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (::mkdir("/etc/swordfs", 0755) != 0 && errno != EEXIST) {
      FAIL() << "failed to create /etc/swordfs: " << strerror(errno);
    }
    std::error_code ec;
    std::filesystem::remove_all("/etc/swordfs/volume-file-round-trip", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-parent-dir", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-exists", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-existing-dir", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-not-dir", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-write-failure", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-lookup-error", ec);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all("/etc/swordfs/volume-file-round-trip", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-parent-dir", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-exists", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-existing-dir", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-not-dir", ec);
    std::filesystem::remove_all("/etc/swordfs/volume-file-write-failure", ec);
    std::filesystem::remove("/etc/swordfs/volume-file-lookup-error", ec);
  }
};

SwordFsVolume MakeVolume() {
  SwordFsVolume volume;
  volume.name = "test-vol-" + std::to_string(::getpid());
  volume.storage = "s3";
  volume.bucket = "s3://endpoint/mybucket/prefix";
  volume.region = "us-east-1";
  return volume;
}
}  // namespace

TEST(SwordFsVolumeTest, SerializeToAndParseFromRoundTrip) {
  SwordFsVolume original = MakeVolume();
  original.chunk_size = 128ULL * 1024 * 1024;

  std::string encoded = original.SerializeTo();
  ASSERT_FALSE(encoded.empty());
  SwordFsVolume parsed;
  Status st = parsed.ParseFrom(encoded);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.storage, original.storage);
  EXPECT_EQ(parsed.bucket, original.bucket);
  EXPECT_EQ(parsed.region, original.region);
  EXPECT_EQ(parsed.chunk_size, original.chunk_size);
  EXPECT_EQ(parsed.chunk_overwrite_strategy, "whole_object");
  EXPECT_EQ(parsed.chunk_index_format_version, 1U);
}

TEST(SwordFsVolumeTest, PersistsStrategyAndRejectsMissingOrZeroVersion) {
  SwordFsVolume volume = MakeVolume();
  volume.chunk_overwrite_strategy = "redis_cache";
  volume.chunk_index_format_version = 7;
  SwordFsVolume parsed;
  ASSERT_TRUE(parsed.ParseFrom(volume.SerializeTo()).ok());
  EXPECT_EQ(parsed.chunk_overwrite_strategy, "redis_cache");
  EXPECT_EQ(parsed.chunk_index_format_version, 7U);

  volume.chunk_overwrite_strategy.clear();
  EXPECT_TRUE(parsed.ParseFrom(volume.SerializeTo()).ToErrno() == EIO);
  volume.chunk_overwrite_strategy = "whole_object";
  volume.chunk_index_format_version = 0;
  EXPECT_TRUE(parsed.ParseFrom(volume.SerializeTo()).ToErrno() == EIO);
}

TEST(SwordFsVolumeTest, ParseFromRejectsMalformedData) {
  SwordFsVolume v;
  Status st = v.ParseFrom("not volume metadata");
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.ToErrno(), EIO);

  SwordFsVolume original = MakeVolume();
  std::string encoded = original.SerializeTo();
  encoded.push_back('\0');
  st = v.ParseFrom(encoded);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.ToErrno(), EIO);
}

TEST(SwordFsVolumeTest, ParseFromRejectsOneSidedDataEngineConfig) {
  SwordFsVolume missing_identity = MakeVolume();
  missing_identity.storage.clear();
  SwordFsVolume parsed;
  Status status = parsed.ParseFrom(missing_identity.SerializeTo());
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();

  SwordFsVolume missing_location = MakeVolume();
  missing_location.bucket.clear();
  status = parsed.ParseFrom(missing_location.SerializeTo());
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
}

TEST(SwordFsVolumeTest, ParseFromRejectsNonCurrentSchemaVersion) {
  for (uint32_t schema_version : {0U, 2U}) {
    swordfs::metadata::BufEncoder enc;
    enc.String("SWFSMETA");
    enc.U32(schema_version);
    enc.U32(static_cast<uint32_t>(swordfs::metadata::RecordType::kVolume));
    enc.String("invalid-schema-volume");
    enc.String("s3");
    enc.String("s3://endpoint/mybucket/prefix");
    enc.String("auto");
    enc.U64(64ULL * 1024 * 1024);

    std::string encoded;
    enc.Finish(&encoded);
    SwordFsVolume volume;
    EXPECT_TRUE(volume.ParseFrom(encoded).ToErrno() == EIO) << "schema=" << schema_version;
  }
}

TEST_F(VolumeFileTest, WriteAndReadRoundTrip) {
  SwordFsVolume original = MakeVolume();
  original.name = "volume-file-round-trip";
  swordfs::metadata::mem::VolumeFile file{original.name};
  Status st = file.Write(original);
  ASSERT_TRUE(st.ok()) << st.message();

  SwordFsVolume restored;
  st = file.Read(&restored);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(restored.name, original.name);
  EXPECT_EQ(restored.storage, original.storage);
  EXPECT_EQ(restored.bucket, original.bucket);
  EXPECT_EQ(restored.region, original.region);
}

TEST_F(VolumeFileTest, WriteCreatesParentDir) {
  SwordFsVolume v = MakeVolume();
  v.name = "volume-file-parent-dir";
  swordfs::metadata::mem::VolumeFile file{v.name};
  Status st = file.Write(v);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_TRUE(file.Exists());
}

TEST_F(VolumeFileTest, ReadNotFound) {
  SwordFsVolume v;
  swordfs::metadata::mem::VolumeFile file{"nonexistent-volume-file"};
  Status st = file.Read(&v);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.ToErrno(), ENOENT);
}

TEST_F(VolumeFileTest, Exists) {
  SwordFsVolume v = MakeVolume();
  v.name = "volume-file-exists";
  swordfs::metadata::mem::VolumeFile file{v.name};
  EXPECT_FALSE(file.Exists());
  ASSERT_TRUE(file.Write(v).ok());
  EXPECT_TRUE(file.Exists());
}

TEST_F(VolumeFileTest, ReadRejectsNullOutput) {
  swordfs::metadata::mem::VolumeFile file{"volume-file-null-output"};
  EXPECT_EQ(file.Read(nullptr).ToErrno(), EINVAL);
}

TEST_F(VolumeFileTest, WriteReusesExistingVolumeDirectory) {
  SwordFsVolume volume = MakeVolume();
  volume.name = "volume-file-existing-dir";
  swordfs::metadata::mem::VolumeFile file{volume.name};

  ASSERT_TRUE(file.Write(volume).ok());
  volume.region = "eu-west-1";
  ASSERT_TRUE(file.Write(volume).ok());

  SwordFsVolume restored;
  ASSERT_TRUE(file.Read(&restored).ok());
  EXPECT_EQ(restored.region, "eu-west-1");
}

TEST_F(VolumeFileTest, WriteRejectsNonDirectoryVolumePath) {
  const std::string path = "/etc/swordfs/volume-file-not-dir";
  ASSERT_TRUE(std::filesystem::is_directory("/etc/swordfs"));
  ASSERT_TRUE(std::ofstream(path).put('x').good());

  SwordFsVolume volume = MakeVolume();
  volume.name = "volume-file-not-dir";
  swordfs::metadata::mem::VolumeFile file{volume.name};
  EXPECT_EQ(file.Write(volume).ToErrno(), EINVAL);
}

TEST_F(VolumeFileTest, WriteReportsVolumeDirectoryLookupFailure) {
  const std::string path = "/etc/swordfs/volume-file-lookup-error";
  ASSERT_EQ(::symlink(path.c_str(), path.c_str()), 0) << strerror(errno);

  SwordFsVolume volume = MakeVolume();
  volume.name = "volume-file-lookup-error";
  swordfs::metadata::mem::VolumeFile file{volume.name};
  EXPECT_EQ(file.Write(volume).ToErrno(), EIO);
}

TEST_F(VolumeFileTest, WriteReportsConfigFileWriteFailure) {
  const std::string directory = "/etc/swordfs/volume-file-write-failure";
  ASSERT_TRUE(std::filesystem::create_directories(directory + "/volume.fmt"));

  SwordFsVolume volume = MakeVolume();
  volume.name = "volume-file-write-failure";
  swordfs::metadata::mem::VolumeFile file{volume.name};
  EXPECT_EQ(file.Write(volume).ToErrno(), EIO);
}
