// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include "metadata/Types.hpp"
#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {
namespace {

SwordFsInode MakeInode() {
  SwordFsInode inode;
  inode.ino = 42;
  inode.attr.dev = 1;
  inode.attr.ino = 42;
  inode.attr.mode = S_IFREG | 0640;
  inode.attr.nlink = 2;
  inode.attr.uid = 1000;
  inode.attr.gid = 1001;
  inode.attr.rdev = 2;
  inode.attr.size = 12345;
  inode.attr.blksize = 4096;
  inode.attr.blocks = 32;
  inode.attr.atime = 10;
  inode.attr.atime_nsec = 11;
  inode.attr.mtime = 20;
  inode.attr.mtime_nsec = 21;
  inode.attr.ctime = 30;
  inode.attr.ctime_nsec = 31;
  inode.parent_ino = 7;
  return inode;
}

}  // namespace

TEST(MetadataTypesTest, InodeRoundTrip) {
  const auto input = MakeInode();
  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsInode output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(output.ino, input.ino);
  EXPECT_EQ(output.attr.dev, input.attr.dev);
  EXPECT_EQ(output.attr.ino, input.attr.ino);
  EXPECT_EQ(output.attr.mode, input.attr.mode);
  EXPECT_EQ(output.attr.nlink, input.attr.nlink);
  EXPECT_EQ(output.attr.uid, input.attr.uid);
  EXPECT_EQ(output.attr.gid, input.attr.gid);
  EXPECT_EQ(output.attr.rdev, input.attr.rdev);
  EXPECT_EQ(output.attr.size, input.attr.size);
  EXPECT_EQ(output.attr.blksize, input.attr.blksize);
  EXPECT_EQ(output.attr.blocks, input.attr.blocks);
  EXPECT_EQ(output.attr.atime, input.attr.atime);
  EXPECT_EQ(output.attr.atime_nsec, input.attr.atime_nsec);
  EXPECT_EQ(output.attr.mtime, input.attr.mtime);
  EXPECT_EQ(output.attr.mtime_nsec, input.attr.mtime_nsec);
  EXPECT_EQ(output.attr.ctime, input.attr.ctime);
  EXPECT_EQ(output.attr.ctime_nsec, input.attr.ctime_nsec);
  EXPECT_EQ(output.parent_ino, input.parent_ino);
}

TEST(MetadataTypesTest, SymlinkRoundTrip) {
  auto input = MakeInode();
  input.attr.mode = S_IFLNK | 0777;
  input.symlink_target = "/some/target";

  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsInode output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(output.symlink_target, input.symlink_target);
}

TEST(MetadataTypesTest, EntryRoundTrip) {
  SwordFsEntry input{.name = "a:b", .type = DT_DIR, .ino = 42};
  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsEntry output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(output.name, input.name);
  EXPECT_EQ(output.type, input.type);
  EXPECT_EQ(output.ino, input.ino);
}

TEST(MetadataTypesTest, ChunkRoundTrip) {
  SwordFsChunk input{.index = 3, .start_offset = 4096, .key = "42/3", .size = 1024};
  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsChunk output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(output.index, input.index);
  EXPECT_EQ(output.start_offset, input.start_offset);
  EXPECT_EQ(output.key, input.key);
  EXPECT_EQ(output.size, input.size);
}

TEST(MetadataTypesTest, DecoderReportsMalformedInput) {
  BufDecoder dec("\x01\x02\x03");
  uint64_t value = 0;
  EXPECT_FALSE(dec.U64(&value));
  EXPECT_FALSE(dec);
}

TEST(MetadataTypesTest, RejectsMalformedHeaderAndString) {
  BufEncoder enc;
  enc.Header(RecordType::kInode);
  enc.U64(4);
  std::string encoded;
  enc.Finish(&encoded);

  BufDecoder truncated(encoded.substr(0, encoded.size() - 1));
  EXPECT_FALSE(truncated.Header(RecordType::kInode));

  BufEncoder string_enc;
  string_enc.U64(100);
  std::string string_data;
  string_enc.Finish(&string_data);
  BufDecoder string_dec(string_data);
  std::string value;
  EXPECT_FALSE(string_dec.String(&value));

  BufEncoder bad_attr_enc;
  SwordFsAttr attr;
  attr.atime_nsec = 1000000000;
  bad_attr_enc.Attr(attr);
  std::string attr_data;
  bad_attr_enc.Finish(&attr_data);
  BufDecoder attr_dec(attr_data);
  SwordFsAttr decoded;
  EXPECT_FALSE(attr_dec.Attr(&decoded));
}

TEST(MetadataTypesTest, RejectsWrongTypeAndMalformedRecords) {
  const auto input = MakeInode();
  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsEntry entry;
  EXPECT_FALSE(entry.ParseFrom(encoded).ok());

  SwordFsInode output;
  EXPECT_FALSE(output.ParseFrom(encoded.substr(0, encoded.size() - 1)).ok());

  encoded.push_back('\0');
  EXPECT_FALSE(output.ParseFrom(encoded).ok());
}

}  // namespace swordfs::metadata
