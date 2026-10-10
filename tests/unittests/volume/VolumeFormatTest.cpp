// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Backend-neutral tests for the canonical SwordFsVolume record.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::ChunkType;
using swordfs::metadata::SwordFsVolume;
using swordfs::utils::Status;

namespace {
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
  EXPECT_EQ(parsed.chunk_type, ChunkType::kCow);
  EXPECT_FALSE(parsed.enable_posix_acl);
}

TEST(SwordFsVolumeTest, PersistsPosixAclFeatureInCanonicalVolumeRecord) {
  const SwordFsVolume volume = MakeVolume();

  swordfs::metadata::BufEncoder enc;
  enc.Header(swordfs::metadata::RecordType::kVolume);
  enc.String(volume.name);
  enc.String(volume.storage);
  enc.String(volume.bucket);
  enc.String(volume.region);
  enc.U64(volume.chunk_size);
  enc.U32(static_cast<uint32_t>(volume.chunk_type));
  enc.U32(1);
  std::string encoded;
  enc.Finish(&encoded);

  SwordFsVolume parsed;
  ASSERT_TRUE(parsed.ParseFrom(encoded).ok());
  EXPECT_TRUE(parsed.enable_posix_acl);
  EXPECT_EQ(parsed.SerializeTo(), encoded);
}

TEST(SwordFsVolumeTest, PersistsChunkTypeAndRejectsUnknownChunkType) {
  SwordFsVolume volume = MakeVolume();
  volume.chunk_type = ChunkType::kRedisCache;
  SwordFsVolume parsed;
  ASSERT_TRUE(parsed.ParseFrom(volume.SerializeTo()).ok());
  EXPECT_EQ(parsed.chunk_type, ChunkType::kRedisCache);

  volume.chunk_type = static_cast<ChunkType>(99);
  EXPECT_TRUE(parsed.ParseFrom(volume.SerializeTo()).ToErrno() == EIO);
}

TEST(SwordFsVolumeTest, ParsesCanonicalChunkTypeNames) {
  ChunkType chunk_type;
  EXPECT_TRUE(swordfs::metadata::ParseChunkType("cow", &chunk_type).ok());
  EXPECT_EQ(chunk_type, ChunkType::kCow);
  EXPECT_TRUE(swordfs::metadata::ParseChunkType("chunk_slice", &chunk_type).ok());
  EXPECT_EQ(chunk_type, ChunkType::kChunkSlice);
  EXPECT_TRUE(swordfs::metadata::ParseChunkType("redis_cache", &chunk_type).ok());
  EXPECT_EQ(chunk_type, ChunkType::kRedisCache);
  EXPECT_EQ(swordfs::metadata::ParseChunkType("whole_object", &chunk_type).ToErrno(), EINVAL);
  EXPECT_EQ(swordfs::metadata::ParseChunkType("unknown", &chunk_type).ToErrno(), EINVAL);
  EXPECT_EQ(swordfs::metadata::ParseChunkType("cow", nullptr).ToErrno(), EINVAL);

  EXPECT_EQ(swordfs::metadata::ChunkTypeName(ChunkType::kCow), "cow");
  EXPECT_EQ(swordfs::metadata::ChunkTypeName(ChunkType::kChunkSlice), "chunk_slice");
  EXPECT_EQ(swordfs::metadata::ChunkTypeName(ChunkType::kRedisCache), "redis_cache");
  EXPECT_TRUE(swordfs::metadata::ChunkTypeName(static_cast<ChunkType>(99)).empty());
  EXPECT_EQ(swordfs::metadata::ChunkTypeKey(ChunkType::kCow), "1");
  EXPECT_EQ(swordfs::metadata::ChunkTypeKey(ChunkType::kChunkSlice), "2");
  EXPECT_EQ(swordfs::metadata::ChunkTypeKey(ChunkType::kRedisCache), "3");
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
  for (uint32_t schema_version : {0U, 1U, 3U}) {
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
