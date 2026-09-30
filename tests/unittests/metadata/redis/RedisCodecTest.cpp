// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cerrno>

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
  inode.attr.btime = 40;
  inode.attr.btime_nsec = 41;
  inode.attr.inode_flags = InodeFlag::kImmutable | InodeFlag::kAppendOnly;
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
  EXPECT_EQ(output.attr.btime, input.attr.btime);
  EXPECT_EQ(output.attr.btime_nsec, input.attr.btime_nsec);
  EXPECT_EQ(output.attr.inode_flags, input.attr.inode_flags);
  EXPECT_EQ(output.parent_ino, input.parent_ino);
}

TEST(MetadataTypesTest, AttrRejectsTruncatedBirthTimeFields) {
  const auto input = MakeInode();
  BufEncoder enc;
  enc.Attr(input.attr);
  std::string encoded;
  enc.Finish(&encoded);

  for (const size_t removed_bytes : {size_t{1}, sizeof(int64_t) + 1}) {
    ASSERT_GT(encoded.size(), removed_bytes);
    BufDecoder dec(std::string_view(encoded).substr(0, encoded.size() - removed_bytes));
    SwordFsAttr output;
    EXPECT_FALSE(dec.Attr(&output)) << "removed_bytes=" << removed_bytes;
  }
}

TEST(MetadataTypesTest, AttrRejectsUnknownPersistedInodeFlagBits) {
  auto input = MakeInode();
  input.attr.inode_flags = static_cast<InodeFlag>(1u << 31);
  BufEncoder enc;
  enc.Attr(input.attr);
  std::string encoded;
  enc.Finish(&encoded);

  BufDecoder dec(encoded);
  SwordFsAttr output;
  EXPECT_FALSE(dec.Attr(&output));
}

TEST(MetadataTypesTest, AttrProjectsAuthoritativeStatxFields) {
  const auto input = MakeInode();
  struct statx result{};

  input.attr.ToStatX(&result);

  EXPECT_EQ(result.stx_mask, STATX_BASIC_STATS | STATX_BTIME);
  EXPECT_EQ(result.stx_attributes, STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND);
  EXPECT_EQ(result.stx_attributes_mask, STATX_ATTR_IMMUTABLE | STATX_ATTR_APPEND);
  EXPECT_EQ(result.stx_blksize, input.attr.blksize);
  EXPECT_EQ(result.stx_ino, input.attr.ino);
  EXPECT_EQ(result.stx_mode, input.attr.mode);
  EXPECT_EQ(result.stx_nlink, input.attr.nlink);
  EXPECT_EQ(result.stx_uid, input.attr.uid);
  EXPECT_EQ(result.stx_gid, input.attr.gid);
  EXPECT_EQ(result.stx_size, input.attr.size);
  EXPECT_EQ(result.stx_blocks, input.attr.blocks);
  EXPECT_EQ(result.stx_atime.tv_sec, input.attr.atime);
  EXPECT_EQ(result.stx_atime.tv_nsec, input.attr.atime_nsec);
  EXPECT_EQ(result.stx_mtime.tv_sec, input.attr.mtime);
  EXPECT_EQ(result.stx_mtime.tv_nsec, input.attr.mtime_nsec);
  EXPECT_EQ(result.stx_ctime.tv_sec, input.attr.ctime);
  EXPECT_EQ(result.stx_ctime.tv_nsec, input.attr.ctime_nsec);
  EXPECT_EQ(result.stx_btime.tv_sec, input.attr.btime);
  EXPECT_EQ(result.stx_btime.tv_nsec, input.attr.btime_nsec);
  EXPECT_EQ(result.stx_dev_major, major(input.attr.dev));
  EXPECT_EQ(result.stx_dev_minor, minor(input.attr.dev));
  EXPECT_EQ(result.stx_rdev_major, major(input.attr.rdev));
  EXPECT_EQ(result.stx_rdev_minor, minor(input.attr.rdev));
}

TEST(MetadataTypesTest, StatxSharesRegularFileBlockProjectionWithStat) {
  SwordFsAttr attr(42, S_IFREG | 0644);
  attr.size = 4096;
  attr.blocks = 0;

  struct stat posix{};
  struct statx extended{};
  attr.ToPosixStat(&posix);
  attr.ToStatX(&extended);

  EXPECT_EQ(extended.stx_blocks, static_cast<uint64_t>(posix.st_blocks));
}

TEST(MetadataTypesTest, InodeXAttrsRoundTripPreservesBinaryValues) {
  auto input = MakeInode();
  input.xattrs.emplace("user.alpha", std::string("a\0b", 3));
  input.xattrs.emplace("user.empty", std::string{});

  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsInode output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(output.xattrs, input.xattrs);
}

TEST(MetadataTypesTest, InodeXAttrEncodingIsDeterministicByName) {
  auto first = MakeInode();
  first.xattrs.emplace("user.zeta", "last");
  first.xattrs.emplace("user.alpha", "first");

  auto second = MakeInode();
  second.xattrs.emplace("user.alpha", "first");
  second.xattrs.emplace("user.zeta", "last");

  std::string first_encoded;
  std::string second_encoded;
  ASSERT_TRUE(first.SerializeTo(&first_encoded).ok());
  ASSERT_TRUE(second.SerializeTo(&second_encoded).ok());
  EXPECT_EQ(first_encoded, second_encoded);
}

TEST(MetadataTypesTest, InodeXAttrHelpersValidateArgumentsAndModes) {
  auto inode = MakeInode();
  const auto original_ctime = inode.attr.ctime;
  const auto original_ctime_nsec = inode.attr.ctime_nsec;

  EXPECT_EQ(inode.SetXAttr("", "value", XAttrSetMode::kUpsert).ToErrno(), EINVAL);
  EXPECT_EQ(inode.SetXAttr("user.key", "value", static_cast<XAttrSetMode>(255)).ToErrno(), EINVAL);
  EXPECT_EQ(inode.attr.ctime, original_ctime);
  EXPECT_EQ(inode.attr.ctime_nsec, original_ctime_nsec);
  EXPECT_EQ(inode.GetXAttr("user.key", nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(inode.ListXAttrs(nullptr).ToErrno(), EINVAL);
}

TEST(MetadataTypesTest, InodeXAttrLimitsBoundIndividualStateAndKeepListQueryable) {
  constexpr size_t kMaxXAttrNameLength = 255;
  constexpr size_t kMaxXAttrValueSize = 64 * 1024;
  constexpr size_t kMaxXAttrListSize = 64 * 1024;

  auto inode = MakeInode();
  const std::string max_name = std::string("user.") + std::string(kMaxXAttrNameLength - 5, 'n');
  const std::string over_name = max_name + "n";
  const std::string max_value(kMaxXAttrValueSize, 'v');
  const std::string over_value(kMaxXAttrValueSize + 1, 'v');

  ASSERT_TRUE(inode.SetXAttr("user.small", "x", XAttrSetMode::kUpsert).ok());
  ASSERT_TRUE(inode.SetXAttr(max_name, max_value, XAttrSetMode::kUpsert).ok());
  EXPECT_EQ(inode.SetXAttr(over_name, "value", XAttrSetMode::kUpsert).ToErrno(), ERANGE);
  EXPECT_EQ(inode.SetXAttr("user.too-large", over_value, XAttrSetMode::kUpsert).ToErrno(), ERANGE);
  ASSERT_TRUE(inode.SetXAttr("user.additional", "x", XAttrSetMode::kUpsert).ok());
  std::string value;
  EXPECT_EQ(inode.GetXAttr(over_name, &value).ToErrno(), ERANGE);
  EXPECT_EQ(inode.RemoveXAttr(over_name).ToErrno(), ERANGE);

  auto list_bound_inode = MakeInode();
  auto make_max_name = [](size_t index) {
    std::string suffix = std::to_string(index);
    suffix.insert(0, 6 - suffix.size(), '0');
    return std::string("user.") + std::string(244, 'n') + suffix;
  };
  constexpr size_t kPackedMaxNameSize = kMaxXAttrNameLength + 1;
  static_assert(kMaxXAttrListSize % kPackedMaxNameSize == 0);
  constexpr size_t kNamesAtListLimit = kMaxXAttrListSize / kPackedMaxNameSize;
  for (size_t i = 0; i < kNamesAtListLimit; ++i) {
    ASSERT_TRUE(list_bound_inode.SetXAttr(make_max_name(i), "", XAttrSetMode::kUpsert).ok()) << i;
  }
  EXPECT_EQ(list_bound_inode.SetXAttr("user.missing", "value", XAttrSetMode::kReplaceOnly).ToErrno(), ENODATA);
  EXPECT_EQ(list_bound_inode.SetXAttr(make_max_name(kNamesAtListLimit), "", XAttrSetMode::kUpsert).ToErrno(), ERANGE);
}

TEST(MetadataTypesTest, InodeXAttrDecoderRejectsDuplicateNames) {
  const auto inode = MakeInode();
  BufEncoder enc;
  enc.Header(RecordType::kInode);
  enc.U64(inode.ino);
  enc.Attr(inode.attr);
  enc.U64(inode.parent_ino);
  enc.String(inode.symlink_target);
  enc.U64(2);
  enc.String("user.duplicate");
  enc.String("first");
  enc.String("user.duplicate");
  enc.String("second");

  std::string encoded;
  enc.Finish(&encoded);
  SwordFsInode parsed;
  EXPECT_EQ(parsed.ParseFrom(encoded).ToErrno(), EIO);
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
  SwordFsChunk input{.index = 3, .revision = 7, .size = 1024};
  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsChunk output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(output.index, input.index);
  EXPECT_EQ(output.revision, input.revision);
  EXPECT_EQ(output.size, input.size);
}

TEST(MetadataTypesTest, ChunkRoundTripPreservesIndexAboveUint32Max) {
  constexpr uint64_t kHighIndex = (uint64_t{1} << 32) + 17;
  SwordFsChunk input{.index = static_cast<ChunkIndex>(kHighIndex), .revision = 9, .size = 1024};
  ASSERT_EQ(static_cast<uint64_t>(input.index), kHighIndex);

  std::string encoded;
  ASSERT_TRUE(input.SerializeTo(&encoded).ok());

  SwordFsChunk output;
  ASSERT_TRUE(output.ParseFrom(encoded).ok());
  EXPECT_EQ(static_cast<uint64_t>(output.index), kHighIndex);
  EXPECT_EQ(output.revision, input.revision);
  EXPECT_EQ(output.size, input.size);
}

TEST(MetadataTypesTest, ChunkRejectsInvalidRevision) {
  SwordFsChunk chunk{.index = 3, .revision = swordfs::metadata::kInvalidChunkRevision, .size = 1024};
  std::string encoded;
  EXPECT_EQ(chunk.SerializeTo(&encoded).ToErrno(), EINVAL);

  swordfs::metadata::BufEncoder enc;
  enc.Header(swordfs::metadata::RecordType::kChunk);
  enc.U64(3);
  enc.U64(swordfs::metadata::kInvalidChunkRevision);
  enc.U64(1024);
  enc.Finish(&encoded);

  SwordFsChunk parsed;
  EXPECT_TRUE(parsed.ParseFrom(encoded).ToErrno() == EIO);
}

TEST(MetadataTypesTest, ChunkRejectsNonCurrentSchemaVersion) {
  for (uint32_t schema_version : {0U, 2U}) {
    BufEncoder enc;
    enc.String("SWFSMETA");
    enc.U32(schema_version);
    enc.U32(static_cast<uint32_t>(RecordType::kChunk));
    enc.U64(3);
    enc.U64(7);
    enc.U64(1024);

    std::string encoded;
    enc.Finish(&encoded);
    SwordFsChunk parsed;
    EXPECT_TRUE(parsed.ParseFrom(encoded).ToErrno() == EIO) << "schema=" << schema_version;
  }
}

TEST(MetadataTypesTest, EncoderFinishOverwritesExistingOutput) {
  BufEncoder enc;
  enc.U64(42);

  std::string encoded = "stale-data";
  enc.Finish(&encoded);

  BufDecoder dec(encoded);
  uint64_t value = 0;
  ASSERT_TRUE(dec.U64(&value));
  EXPECT_EQ(value, 42U);
  EXPECT_TRUE(dec.Done());
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

  for (const int64_t invalid_btime_nsec : {-1LL, 1000000000LL}) {
    BufEncoder bad_btime_enc;
    attr = {};
    attr.btime_nsec = invalid_btime_nsec;
    bad_btime_enc.Attr(attr);
    bad_btime_enc.Finish(&attr_data);
    BufDecoder btime_dec(attr_data);
    EXPECT_FALSE(btime_dec.Attr(&decoded));
  }
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
