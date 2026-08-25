// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <cstring>

#include "metadata/redis/RedisCodec.hpp"

namespace swordfs::metadata {
namespace {

SwordFsInode MakeInode() {
  SwordFsInode inode;
  inode.ino = 42;
  inode.attr.st_ino = 42;
  inode.attr.st_mode = S_IFREG | 0640;
  inode.attr.st_nlink = 2;
  inode.attr.st_uid = 1000;
  inode.attr.st_gid = 1001;
  inode.attr.st_size = 12345;
  inode.attr.st_blksize = 4096;
  inode.attr.st_blocks = 32;
  inode.attr.st_atim.tv_sec = 10;
  inode.attr.st_atim.tv_nsec = 11;
  inode.attr.st_mtim.tv_sec = 20;
  inode.attr.st_mtim.tv_nsec = 21;
  inode.attr.st_ctim.tv_sec = 30;
  inode.attr.st_ctim.tv_nsec = 31;
  inode.parent_ino = 7;
  inode.symlink_target.clear();
  return inode;
}

}  // namespace

TEST(RedisCodecTest, FormatRoundTrip) {
  RedisFormat input{.schema_version = RedisCodec::kSchemaVersion};
  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeFormat(input, &encoded).ok());

  RedisFormat output;
  ASSERT_TRUE(RedisCodec::DecodeFormat(encoded, &output).ok());
  EXPECT_EQ(output.schema_version, input.schema_version);
}

TEST(RedisCodecTest, InodeRoundTrip) {
  const auto input = MakeInode();
  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeInode(input, &encoded).ok());

  SwordFsInode output;
  ASSERT_TRUE(RedisCodec::DecodeInode(encoded, &output).ok());
  EXPECT_EQ(output.ino, input.ino);
  EXPECT_EQ(output.attr.st_ino, input.attr.st_ino);
  EXPECT_EQ(output.attr.st_mode, input.attr.st_mode);
  EXPECT_EQ(output.attr.st_nlink, input.attr.st_nlink);
  EXPECT_EQ(output.attr.st_uid, input.attr.st_uid);
  EXPECT_EQ(output.attr.st_gid, input.attr.st_gid);
  EXPECT_EQ(output.attr.st_size, input.attr.st_size);
  EXPECT_EQ(output.attr.st_blksize, input.attr.st_blksize);
  EXPECT_EQ(output.attr.st_blocks, input.attr.st_blocks);
  EXPECT_EQ(output.attr.st_atim.tv_sec, input.attr.st_atim.tv_sec);
  EXPECT_EQ(output.attr.st_atim.tv_nsec, input.attr.st_atim.tv_nsec);
  EXPECT_EQ(output.attr.st_mtim.tv_sec, input.attr.st_mtim.tv_sec);
  EXPECT_EQ(output.attr.st_mtim.tv_nsec, input.attr.st_mtim.tv_nsec);
  EXPECT_EQ(output.attr.st_ctim.tv_sec, input.attr.st_ctim.tv_sec);
  EXPECT_EQ(output.attr.st_ctim.tv_nsec, input.attr.st_ctim.tv_nsec);
  EXPECT_EQ(output.parent_ino, input.parent_ino);
}

TEST(RedisCodecTest, SymlinkRoundTrip) {
  auto input = MakeInode();
  input.attr.st_mode = S_IFLNK | 0777;
  input.symlink_target = "/some/target";

  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeInode(input, &encoded).ok());

  SwordFsInode output;
  ASSERT_TRUE(RedisCodec::DecodeInode(encoded, &output).ok());
  EXPECT_EQ(output.symlink_target, input.symlink_target);
}

TEST(RedisCodecTest, EntryRoundTrip) {
  SwordFsEntry input{.name = "a:b", .type = DT_DIR, .ino = 42};
  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeEntry(input, &encoded).ok());

  SwordFsEntry output;
  ASSERT_TRUE(RedisCodec::DecodeEntry(encoded, &output).ok());
  EXPECT_EQ(output.name, input.name);
  EXPECT_EQ(output.type, input.type);
  EXPECT_EQ(output.ino, input.ino);
}

TEST(RedisCodecTest, ChunkRoundTrip) {
  ChunkMeta input{.index = 3, .start_offset = 4096, .key = "42/3", .size = 1024};
  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeChunk(input, &encoded).ok());

  ChunkMeta output;
  ASSERT_TRUE(RedisCodec::DecodeChunk(encoded, &output).ok());
  EXPECT_EQ(output.index, input.index);
  EXPECT_EQ(output.start_offset, input.start_offset);
  EXPECT_EQ(output.key, input.key);
  EXPECT_EQ(output.size, input.size);
}

TEST(RedisCodecTest, RejectsTruncatedAndUnknownVersionRecords) {
  const auto input = MakeInode();
  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeInode(input, &encoded).ok());

  SwordFsInode output;
  EXPECT_FALSE(RedisCodec::DecodeInode(encoded.substr(0, encoded.size() - 1), &output).ok());

  encoded[12] = static_cast<char>(2);
  EXPECT_FALSE(RedisCodec::DecodeInode(encoded, &output).ok());
}

TEST(RedisCodecTest, RejectsTrailingBytes) {
  const auto input = MakeInode();
  std::string encoded;
  ASSERT_TRUE(RedisCodec::EncodeInode(input, &encoded).ok());
  encoded.push_back('\0');

  SwordFsInode output;
  EXPECT_FALSE(RedisCodec::DecodeInode(encoded, &output).ok());
}

}  // namespace swordfs::metadata
