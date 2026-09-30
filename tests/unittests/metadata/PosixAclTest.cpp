// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "metadata/PosixAcl.hpp"
#include "metadata/PosixAclTestSupport.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Inode.hpp"

namespace {

namespace acl_test = swordfs::test::posix_acl;
using swordfs::metadata::ApplyPosixAclCreateInheritance;
using swordfs::metadata::kMaxXAttrListSize;
using swordfs::metadata::kMaxXAttrValueSize;
using swordfs::metadata::kPosixAclAccessXAttr;
using swordfs::metadata::kPosixAclDefaultXAttr;
using swordfs::metadata::RemovePosixAclXAttr;
using swordfs::metadata::SetPosixAclXAttr;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SyncPosixAccessAclForMode;
using swordfs::metadata::XAttrSetMode;

SwordFsInode MakeInode(uint32_t mode) {
  constexpr uint64_t kIno = 42;
  constexpr uint64_t kParentIno = 1;
  return SwordFsInode(kIno, SwordFsAttr(kIno, mode, 1000, 1000), kParentIno);
}

std::string MinimalAcl(uint16_t owner = 6, uint16_t group = 4, uint16_t other = 0) {
  return acl_test::Encode({
      {acl_test::kUserObj, owner},
      {acl_test::kGroupObj, group},
      {acl_test::kOther, other},
  });
}

std::string ExtendedAcl() {
  return acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kUser, 6, 1001},
      {acl_test::kGroupObj, 5},
      {acl_test::kGroup, 4, 2001},
      {acl_test::kMask, 5},
      {acl_test::kOther, 1},
  });
}

TEST(PosixAclTest, RejectsMalformedEntryOrderingAndRequiredEntryViolations) {
  auto expect_invalid = [](std::string value) {
    auto inode = MakeInode(S_IFREG | 0644);
    EXPECT_EQ(SetPosixAclXAttr(&inode, kPosixAclAccessXAttr, value, XAttrSetMode::kUpsert).ToErrno(), EINVAL);
  };

  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kGroupObj, 5},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kUserObj, 6},
      {acl_test::kGroupObj, 5},
      {acl_test::kOther, 1},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kGroupObj, 5},
      {acl_test::kUser, 6, 1001},
      {acl_test::kMask, 5},
      {acl_test::kOther, 1},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kGroup, 4, 2001},
      {acl_test::kGroupObj, 5},
      {acl_test::kMask, 5},
      {acl_test::kOther, 1},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kGroupObj, 5},
      {acl_test::kGroup, 4, 2002},
      {acl_test::kGroup, 4, 2001},
      {acl_test::kMask, 5},
      {acl_test::kOther, 1},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kMask, 5},
      {acl_test::kGroupObj, 5},
      {acl_test::kOther, 1},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kOther, 1},
      {acl_test::kGroupObj, 5},
  }));
  expect_invalid(acl_test::Encode({
      {acl_test::kUserObj, 7},
      {acl_test::kGroupObj, 5},
      {acl_test::kMask, 5},
  }));
}

TEST(PosixAclTest, MutationBoundaryCoversSetModesPlacementAndResourceLimits) {
  const std::string extended = ExtendedAcl();
  auto file = MakeInode(S_IFREG | 0644);

  EXPECT_EQ(SetPosixAclXAttr(nullptr, kPosixAclAccessXAttr, extended, XAttrSetMode::kUpsert).ToErrno(), EINVAL);
  EXPECT_EQ(SetPosixAclXAttr(&file, "user.acl", extended, XAttrSetMode::kUpsert).ToErrno(), EINVAL);
  EXPECT_EQ(
      SetPosixAclXAttr(&file, kPosixAclAccessXAttr, std::string(kMaxXAttrValueSize + 1, 'x'), XAttrSetMode::kUpsert)
          .ToErrno(),
      ERANGE);
  EXPECT_EQ(SetPosixAclXAttr(&file, kPosixAclDefaultXAttr, extended, XAttrSetMode::kUpsert).ToErrno(), EACCES);

  ASSERT_TRUE(SetPosixAclXAttr(&file, kPosixAclAccessXAttr, extended, XAttrSetMode::kCreateOnly).ok());
  EXPECT_EQ(SetPosixAclXAttr(&file, kPosixAclAccessXAttr, extended, XAttrSetMode::kCreateOnly).ToErrno(), EEXIST);
  ASSERT_TRUE(SetPosixAclXAttr(&file, kPosixAclAccessXAttr, extended, XAttrSetMode::kReplaceOnly).ok());
  EXPECT_EQ(SetPosixAclXAttr(&file, kPosixAclAccessXAttr, extended, static_cast<XAttrSetMode>(99)).ToErrno(), EINVAL);

  auto full = MakeInode(S_IFREG | 0644);
  constexpr size_t kStoredNameLength = 250;
  for (size_t i = 0; i < 262; ++i) {
    std::string name = "user." + std::to_string(i);
    name.append(kStoredNameLength - name.size(), 'x');
    full.xattrs.emplace(std::move(name), "v");
  }
  ASSERT_GT(full.xattrs.size() * (kStoredNameLength + 1), kMaxXAttrListSize);
  EXPECT_EQ(SetPosixAclXAttr(&full, kPosixAclAccessXAttr, extended, XAttrSetMode::kUpsert).ToErrno(), ERANGE);
}

TEST(PosixAclTest, RemoveAndModeSyncDefendTheirSemanticBoundary) {
  const std::string extended = ExtendedAcl();
  auto file = MakeInode(S_IFREG | 0644);
  auto link = MakeInode(S_IFLNK | 0777);

  EXPECT_EQ(RemovePosixAclXAttr(nullptr, kPosixAclAccessXAttr).ToErrno(), EINVAL);
  EXPECT_EQ(RemovePosixAclXAttr(&file, "user.acl").ToErrno(), EINVAL);
  EXPECT_EQ(RemovePosixAclXAttr(&link, kPosixAclAccessXAttr).ToErrno(), EOPNOTSUPP);
  EXPECT_EQ(RemovePosixAclXAttr(&file, kPosixAclDefaultXAttr).ToErrno(), EACCES);
  EXPECT_EQ(RemovePosixAclXAttr(&file, kPosixAclAccessXAttr).ToErrno(), ENODATA);

  EXPECT_EQ(SyncPosixAccessAclForMode(nullptr, 0600).ToErrno(), EINVAL);
  ASSERT_TRUE(SyncPosixAccessAclForMode(&file, 0600).ok());

  file.xattrs[std::string(kPosixAclAccessXAttr)] = "malformed";
  EXPECT_EQ(SyncPosixAccessAclForMode(&file, 0600).ToErrno(), EIO);

  file.xattrs[std::string(kPosixAclAccessXAttr)] = MinimalAcl(7, 5, 1);
  ASSERT_TRUE(SyncPosixAccessAclForMode(&file, 0640).ok());
  EXPECT_EQ(file.xattrs[std::string(kPosixAclAccessXAttr)], MinimalAcl(6, 4, 0));

  file.xattrs[std::string(kPosixAclAccessXAttr)] = extended;
  ASSERT_TRUE(RemovePosixAclXAttr(&file, kPosixAclAccessXAttr).ok());
  EXPECT_FALSE(file.xattrs.contains(std::string(kPosixAclAccessXAttr)));
}

TEST(PosixAclTest, CreateInheritanceHandlesMinimalDefaultsUmaskAndMalformedState) {
  auto parent = MakeInode(S_IFDIR | 0777);
  auto child = MakeInode(S_IFREG | 0666);

  EXPECT_EQ(ApplyPosixAclCreateInheritance(parent, 0022, nullptr).ToErrno(), EINVAL);

  auto symlink = MakeInode(S_IFLNK | 0777);
  ASSERT_TRUE(ApplyPosixAclCreateInheritance(parent, 0077, &symlink).ok());
  EXPECT_EQ(symlink.attr.mode & 0777u, 0777u);

  ASSERT_TRUE(ApplyPosixAclCreateInheritance(parent, 0027, &child).ok());
  EXPECT_EQ(child.attr.mode & 0777u, 0640u);

  parent.xattrs[std::string(kPosixAclDefaultXAttr)] = "malformed";
  auto corrupt_child = MakeInode(S_IFREG | 0666);
  EXPECT_EQ(ApplyPosixAclCreateInheritance(parent, 0, &corrupt_child).ToErrno(), EIO);

  const std::string minimal_default = MinimalAcl(7, 5, 1);
  parent.xattrs[std::string(kPosixAclDefaultXAttr)] = minimal_default;
  auto inherited_file = MakeInode(S_IFREG | 0640);
  ASSERT_TRUE(ApplyPosixAclCreateInheritance(parent, 0077, &inherited_file).ok());
  EXPECT_EQ(inherited_file.attr.mode & 0777u, 0640u);
  EXPECT_FALSE(inherited_file.xattrs.contains(std::string(kPosixAclAccessXAttr)));

  auto inherited_dir = MakeInode(S_IFDIR | 0770);
  ASSERT_TRUE(ApplyPosixAclCreateInheritance(parent, 0077, &inherited_dir).ok());
  EXPECT_EQ(inherited_dir.attr.mode & 0777u, 0750u);
  EXPECT_EQ(inherited_dir.xattrs[std::string(kPosixAclDefaultXAttr)], minimal_default);
}

}  // namespace
