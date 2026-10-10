// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "metadata/PosixAclTestSupport.hpp"

// These reusable assertions intentionally know nothing about Redis or Memory.
// A backend fixture owns format/connection lifecycle, then runs them in a fiber.
namespace swordfs::test::meta_contract {

inline void NamespaceRejectsMissingAndPreservesIdentity(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode parent;
  SwordFsInode child;
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "contract-parent", 0755, &parent).ok());
  ASSERT_TRUE(meta.Create(parent.ino, "child", 0644, &child).ok());

  SwordFsInode looked_up;
  EXPECT_TRUE(meta.Rename(parent.ino, "absent", kRootInodeId, "lost", RenameFlag::kNone).IsNotFound());
  EXPECT_TRUE(meta.Rename(parent.ino, "child", 99999999, "lost", RenameFlag::kNone).IsNotFound());
  EXPECT_TRUE(meta.Rename(99999999, "child", parent.ino, "lost", RenameFlag::kExchange).IsNotFound());
  EXPECT_TRUE(meta.Rename(parent.ino, "absent", parent.ino, "child", RenameFlag::kExchange).IsNotFound());
  EXPECT_TRUE(meta.Rename(parent.ino, "child", parent.ino, "absent", RenameFlag::kExchange).IsNotFound());
  ASSERT_TRUE(meta.Lookup(parent.ino, "child", &looked_up).ok());
  EXPECT_EQ(looked_up.ino, child.ino);

  ASSERT_TRUE(meta.Rename(parent.ino, "child", kRootInodeId, "moved", RenameFlag::kNone).ok());
  EXPECT_TRUE(meta.Lookup(parent.ino, "child", &looked_up).IsNotFound());
  ASSERT_TRUE(meta.Lookup(kRootInodeId, "moved", &looked_up).ok());
  EXPECT_EQ(looked_up.ino, child.ino);
  EXPECT_EQ(looked_up.parent_ino, kRootInodeId);
}

inline void DirectoryIterationIsAnIndependentOpaqueView(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode dir;
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "contract-directory", 0755, &dir).ok());

  DirIteratorPtr empty;
  ASSERT_TRUE(meta.OpenDir(dir.ino, &empty).ok());
  ASSERT_NE(empty, nullptr);
  SwordFsEntry entry;
  uint64_t cookie = 0;
  ASSERT_TRUE(empty->Peek(&entry, &cookie).ok());
  EXPECT_EQ(entry.name, ".");
  EXPECT_EQ(entry.ino, dir.ino);
  empty->Advance();
  ASSERT_TRUE(empty->Peek(&entry, &cookie).ok());
  EXPECT_EQ(entry.name, "..");
  EXPECT_EQ(entry.ino, kRootInodeId);
  empty->Advance();
  EXPECT_TRUE(empty->Peek(&entry, &cookie).IsEndOfDirectory());
  DirIteratorPtr missing;
  EXPECT_TRUE(meta.OpenDir(99999999, &missing).IsNotFound());

  for (int i = 0; i < 128; ++i) {
    ASSERT_TRUE(meta.Create(dir.ino, "entry-" + std::to_string(i), 0644, nullptr).ok());
  }
  SwordFsInode child_dir;
  ASSERT_TRUE(meta.MkDir(dir.ino, "directory", 0755, &child_dir).ok());
  ASSERT_TRUE(meta.Symlink(dir.ino, "symlink", "target", nullptr).ok());
  DirIteratorPtr first;
  DirIteratorPtr second;
  ASSERT_TRUE(meta.OpenDir(dir.ino, &first).ok());
  ASSERT_TRUE(meta.OpenDir(dir.ino, &second).ok());
  ASSERT_NE(first, second);

  std::set<std::string> entries;
  std::map<std::string, uint32_t> types;
  for (;;) {
    auto status = first->Peek(&entry, &cookie);
    if (status.IsEndOfDirectory()) {
      break;
    }
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_TRUE(entries.insert(entry.name).second) << entry.name;
    types[entry.name] = entry.type;
    first->Advance();
  }
  EXPECT_EQ(entries.size(), 132U);
  EXPECT_EQ(types["directory"], DT_DIR);
  EXPECT_EQ(types["symlink"], DT_LNK);
  ASSERT_TRUE(second->Peek(&entry, &cookie).ok());
  EXPECT_EQ(entry.name, ".");
  ASSERT_TRUE(first->Seek(0).ok());
  ASSERT_TRUE(first->Peek(&entry, &cookie).ok());
  EXPECT_EQ(entry.name, ".");

  // A newly opened view reflects completed namespace changes without
  // imposing any ordering or mutation-invalidation rule on old iterators.
  ASSERT_TRUE(meta.Rename(dir.ino, "entry-0", dir.ino, "renamed", RenameFlag::kNone).ok());
  ASSERT_TRUE(meta.Unlink(dir.ino, "entry-1").ok());
  DirIteratorPtr fresh;
  ASSERT_TRUE(meta.OpenDir(dir.ino, &fresh).ok());
  std::set<std::string> current;
  for (;;) {
    auto status = fresh->Peek(&entry, &cookie);
    if (status.IsEndOfDirectory()) {
      break;
    }
    ASSERT_TRUE(status.ok()) << status.message();
    current.insert(entry.name);
    fresh->Advance();
  }
  EXPECT_EQ(current.size(), 131U);
  EXPECT_TRUE(current.contains("renamed"));
  EXPECT_FALSE(current.contains("entry-0"));
  EXPECT_FALSE(current.contains("entry-1"));
}

inline void OrphanRevivalAndReclaimAreMutuallyExclusive(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "contract-orphan", 0644, &file).ok());
  ASSERT_TRUE(meta.Unlink(kRootInodeId, "contract-orphan").ok());
  std::vector<InodeID> orphans;
  ASSERT_TRUE(meta.VisitOrphanCandidates([&](InodeID ino) {
                    orphans.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_NE(std::find(orphans.begin(), orphans.end(), file.ino), orphans.end());

  ASSERT_TRUE(meta.Link(file.ino, kRootInodeId, "contract-revived", nullptr).ok());
  ASSERT_TRUE(meta.PrepareReclaim(file.ino).ok());
  SwordFsInode observed;
  ASSERT_TRUE(meta.Lookup(kRootInodeId, "contract-revived", &observed).ok());
  EXPECT_EQ(observed.ino, file.ino);
  ASSERT_TRUE(meta.Unlink(kRootInodeId, "contract-revived").ok());
  ASSERT_TRUE(meta.PrepareReclaim(file.ino).ok());
  EXPECT_TRUE(meta.GetInode(file.ino, &observed).IsNotFound());
  EXPECT_TRUE(meta.Link(file.ino, kRootInodeId, "contract-resurrected", nullptr).IsNotFound());
  EXPECT_TRUE(meta.PrepareReclaim(file.ino).ok());
}

inline void XAttrsValidateCreateReplaceAndMissing(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "contract-xattr", 0644, &file).ok());
  const std::string binary_value("first\0value", 11);
  ASSERT_TRUE(meta.SetXAttr(file.ino, "user.value", binary_value, XAttrSetMode::kCreateOnly).ok());
  EXPECT_EQ(meta.SetXAttr(file.ino, "user.value", "duplicate", XAttrSetMode::kCreateOnly).ToErrno(), EEXIST);
  EXPECT_EQ(meta.SetXAttr(file.ino, "user.absent", "x", XAttrSetMode::kReplaceOnly).ToErrno(), ENODATA);
  std::string value;
  ASSERT_TRUE(meta.GetXAttr(file.ino, "user.value", &value).ok());
  EXPECT_EQ(value, binary_value);
  ASSERT_TRUE(meta.SetXAttr(file.ino, "user.value", "updated", XAttrSetMode::kReplaceOnly).ok());
  ASSERT_TRUE(meta.GetXAttr(file.ino, "user.value", &value).ok());
  EXPECT_EQ(value, "updated");
  ASSERT_TRUE(meta.RemoveXAttr(file.ino, "user.value").ok());
  EXPECT_EQ(meta.GetXAttr(file.ino, "user.value", &value).ToErrno(), ENODATA);
}

inline void RenameRejectsAncestryCycles(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode dir;
  SwordFsInode nested;
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "contract-ancestor", 0755, &dir).ok());
  ASSERT_TRUE(meta.MkDir(dir.ino, "nested", 0755, &nested).ok());
  EXPECT_EQ(meta.Rename(kRootInodeId, "contract-ancestor", dir.ino, "self", RenameFlag::kNone).ToErrno(), EINVAL);
  EXPECT_EQ(meta.Rename(kRootInodeId, "contract-ancestor", nested.ino, "loop", RenameFlag::kNone).ToErrno(), EINVAL);
  EXPECT_EQ(meta.Rename(kRootInodeId, "contract-ancestor", dir.ino, "nested", RenameFlag::kExchange).ToErrno(), EINVAL);
  EXPECT_EQ(meta.Rename(kRootInodeId, "..", kRootInodeId, "escaped", RenameFlag::kNone).ToErrno(), EBUSY);
  SwordFsInode found;
  ASSERT_TRUE(meta.Lookup(kRootInodeId, "contract-ancestor", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  ASSERT_TRUE(meta.Lookup(dir.ino, "nested", &found).ok());
  EXPECT_EQ(found.ino, nested.ino);
}

inline void RenameSameInodeAcrossParentsIsNoOp(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode file;
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "alias-left", 0755, &left).ok());
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "alias-right", 0755, &right).ok());
  ASSERT_TRUE(meta.Create(left.ino, "file", 0644, &file).ok());
  ASSERT_TRUE(meta.Link(file.ino, right.ino, "alias", nullptr).ok());

  SwordFsInode before;
  ASSERT_TRUE(meta.GetInode(file.ino, &before).ok());
  ASSERT_TRUE(meta.Rename(right.ino, "alias", left.ino, "file", RenameFlag::kExchange).ok());

  SwordFsInode found;
  ASSERT_TRUE(meta.Lookup(left.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  ASSERT_TRUE(meta.Lookup(right.ino, "alias", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  SwordFsInode after;
  ASSERT_TRUE(meta.GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.parent_ino, before.parent_ino);
  EXPECT_EQ(after.attr.nlink, before.attr.nlink);
}

inline void RenameExchangePreservesDirectoryChildren(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode first_dir;
  SwordFsInode second_dir;
  SwordFsInode first_child;
  SwordFsInode second_child;
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "exchange-dir-a", 0755, &first_dir).ok());
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "exchange-dir-b", 0755, &second_dir).ok());
  ASSERT_TRUE(meta.Create(first_dir.ino, "a-child", 0644, &first_child).ok());
  ASSERT_TRUE(meta.Create(second_dir.ino, "b-child", 0644, &second_child).ok());

  ASSERT_TRUE(meta.Rename(kRootInodeId, "exchange-dir-a", kRootInodeId, "exchange-dir-b", RenameFlag::kExchange).ok());

  SwordFsInode observed;
  ASSERT_TRUE(meta.Lookup(kRootInodeId, "exchange-dir-a", &observed).ok());
  EXPECT_EQ(observed.ino, second_dir.ino);
  ASSERT_TRUE(meta.Lookup(kRootInodeId, "exchange-dir-b", &observed).ok());
  EXPECT_EQ(observed.ino, first_dir.ino);
  ASSERT_TRUE(meta.Lookup(second_dir.ino, "b-child", &observed).ok());
  EXPECT_EQ(observed.ino, second_child.ino);
  ASSERT_TRUE(meta.Lookup(first_dir.ino, "a-child", &observed).ok());
  EXPECT_EQ(observed.ino, first_child.ino);
}

inline void StickyOwnerExceptionsPreserveNamespace(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode dir;
  ASSERT_TRUE(meta.MkDir(kRootInodeId, "contract-sticky", 01777, &dir).ok());
  SwordFsAttr change;
  change.uid = 2000;
  // Like the kernel's explicit chmod path, request the sticky bit through
  // setattr: mkdir's mode normalization alone does not retain it.
  change.mode = S_IFDIR | 01777;
  ASSERT_TRUE(meta.SetAttr(dir.ino, change, SetAttrField::kUid | SetAttrField::kMode, nullptr).ok());

  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx.uid = 1000;
  ctx.gid = 1000;
  ASSERT_TRUE(meta.Create(dir.ino, "owned-by-file-owner", 0644, nullptr).ok());
  ctx.uid = 3000;
  EXPECT_EQ(meta.Unlink(dir.ino, "owned-by-file-owner").ToErrno(), EACCES);
  ctx.uid = 1000;
  ASSERT_TRUE(meta.Unlink(dir.ino, "owned-by-file-owner").ok());

  ASSERT_TRUE(meta.Create(dir.ino, "owned-by-dir-owner", 0644, nullptr).ok());
  ctx.uid = 2000;
  ASSERT_TRUE(meta.Unlink(dir.ino, "owned-by-dir-owner").ok());

  ctx.uid = 1000;
  ASSERT_TRUE(meta.MkDir(dir.ino, "owned-subdir", 0755, nullptr).ok());
  ASSERT_TRUE(meta.RmDir(dir.ino, "owned-subdir").ok());
  ASSERT_TRUE(meta.Create(dir.ino, "owned-root-override", 0644, nullptr).ok());
  ctx.uid = 0;
  ASSERT_TRUE(meta.Unlink(dir.ino, "owned-root-override").ok());
}

inline void IdempotentInodePolicyDoesNotChangeCtime(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "contract-policy-ctime", 0644, &file).ok());
  SwordFsAttr attr;
  attr.ctime = 1;
  ASSERT_TRUE(meta.SetAttr(file.ino, attr, SetAttrField::kCtime, &file).ok());
  ASSERT_EQ(file.attr.ctime, 1);
  ASSERT_TRUE(meta.SetInodeFlags(file.ino, InodeFlag::kImmutable, &file).ok());
  EXPECT_NE(file.attr.ctime, 1);
  const auto timestamp = file.attr.ctime;
  const auto nanos = file.attr.ctime_nsec;
  ASSERT_TRUE(meta.SetInodeFlags(file.ino, InodeFlag::kImmutable, &file).ok());
  EXPECT_EQ(file.attr.ctime, timestamp);
  EXPECT_EQ(file.attr.ctime_nsec, nanos);
}

inline void AclRejectsMalformedLinuxEncoding(metadata::IMetaEngine &meta) {
  using namespace metadata;
  namespace acl = swordfs::test::posix_acl;
  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "contract-acl-invalid", 0644, &file).ok());

  // A minimal ACL is folded into mode bits and has no persistent xattr.
  // Use an extended ACL to exercise rollback of an actual published payload.
  const auto valid = acl::Encode({
      {acl::kUserObj, 7},
      {acl::kUser, 6, 1001},
      {acl::kGroupObj, 5},
      {acl::kMask, 5},
      {acl::kOther, 5},
  });
  // Rejected replacements must not alter an already-published ACL or mode.
  ASSERT_TRUE(meta.SetXAttr(file.ino, "system.posix_acl_access", valid, XAttrSetMode::kUpsert).ok());
  SwordFsInode before;
  ASSERT_TRUE(meta.GetInode(file.ino, &before).ok());
  std::string published_acl;
  ASSERT_TRUE(meta.GetXAttr(file.ino, "system.posix_acl_access", &published_acl).ok());
  std::string bad_version = valid;
  bad_version[0] = 1;
  std::string truncated = valid;
  truncated.pop_back();
  const std::vector<std::string> malformed = {
      bad_version,
      truncated,
      acl::Encode({{acl::kUserObj, 8}, {acl::kGroupObj, 5}, {acl::kOther, 5}}),
      acl::Encode({{acl::kUserObj, 7, 1000}, {acl::kGroupObj, 5}, {acl::kOther, 5}}),
      acl::Encode({{acl::kUserObj, 7}, {acl::kUser, 6}, {acl::kGroupObj, 5}, {acl::kMask, 5}, {acl::kOther, 5}}),
      acl::Encode({{acl::kGroupObj, 5}, {acl::kUserObj, 7}, {acl::kOther, 5}}),
      acl::Encode({{acl::kUserObj, 7},
                   {acl::kUser, 6, 1001},
                   {acl::kUser, 4, 1001},
                   {acl::kGroupObj, 5},
                   {acl::kMask, 5},
                   {acl::kOther, 5}}),
      acl::Encode({{acl::kUserObj, 7}, {0x40, 5}, {acl::kGroupObj, 5}, {acl::kOther, 5}}),
  };
  for (const auto &encoding : malformed) {
    EXPECT_EQ(meta.SetXAttr(file.ino, "system.posix_acl_access", encoding, XAttrSetMode::kUpsert).ToErrno(), EINVAL);
    SwordFsInode after;
    ASSERT_TRUE(meta.GetInode(file.ino, &after).ok());
    EXPECT_EQ(after.attr.mode, before.attr.mode);
    EXPECT_EQ(after.attr.ctime, before.attr.ctime);
    EXPECT_EQ(after.attr.ctime_nsec, before.attr.ctime_nsec);
    EXPECT_EQ(after.xattrs, before.xattrs);
    std::string observed_acl;
    ASSERT_TRUE(meta.GetXAttr(file.ino, "system.posix_acl_access", &observed_acl).ok());
    EXPECT_EQ(observed_acl, published_acl);
  }
  EXPECT_EQ(meta.SetXAttr(file.ino, "system.posix_acl_default", valid, XAttrSetMode::kUpsert).ToErrno(), EACCES);
  SwordFsInode after;
  ASSERT_TRUE(meta.GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.attr.mode, before.attr.mode);
  EXPECT_EQ(after.attr.ctime, before.attr.ctime);
  EXPECT_EQ(after.attr.ctime_nsec, before.attr.ctime_nsec);
  EXPECT_EQ(after.xattrs, before.xattrs);
  std::string observed_acl;
  ASSERT_TRUE(meta.GetXAttr(file.ino, "system.posix_acl_access", &observed_acl).ok());
  EXPECT_EQ(observed_acl, published_acl);
}

inline void ChunkPublicationValidatesMaximumFileExtent(metadata::IMetaEngine &meta) {
  using namespace metadata;
  constexpr uint64_t kChunkSize = 4096;
  constexpr ChunkIndex kLastIndex = kMaxSupportedFileSize / kChunkSize;
  constexpr uint64_t kLastBytes = kMaxSupportedFileSize - kLastIndex * kChunkSize;

  SwordFsInode exact_file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "contract-exact-off-max", 0644, &exact_file).ok());
  const SwordFsChunk exact{.index = kLastIndex, .revision = 1, .size = kLastBytes};
  ASSERT_TRUE(meta.CommitChunk(exact_file.ino, std::nullopt, exact).ok());
  SwordFsInode published;
  ASSERT_TRUE(meta.GetInode(exact_file.ino, &published).ok());
  EXPECT_EQ(published.attr.size, kMaxSupportedFileSize);

  SwordFsInode beyond_file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "contract-beyond-off-max", 0644, &beyond_file).ok());
  const SwordFsChunk beyond{.index = kLastIndex, .revision = 2, .size = kLastBytes + 1};
  EXPECT_EQ(meta.CommitChunk(beyond_file.ino, std::nullopt, beyond).ToErrno(), EINVAL);
  const SwordFsChunk overflowing{.index = std::numeric_limits<ChunkIndex>::max(), .revision = 3, .size = 1};
  EXPECT_EQ(meta.CommitChunk(beyond_file.ino, std::nullopt, overflowing).ToErrno(), EINVAL);
  SwordFsChunk unpublished;
  EXPECT_TRUE(meta.FindChunk(beyond_file.ino, kLastIndex, &unpublished).IsNotFound());
  ASSERT_TRUE(meta.Truncate(beyond_file.ino, 0).ok());
}

inline void ReadlinkAndReclaimValidateMissingState(metadata::IMetaEngine &meta) {
  using namespace metadata;
  SwordFsInode link;
  ASSERT_TRUE(meta.Symlink(kRootInodeId, "contract-symlink", "target", &link).ok());
  EXPECT_EQ(meta.Readlink(link.ino, nullptr).ToErrno(), EINVAL);
  std::string target;
  ASSERT_TRUE(meta.Readlink(link.ino, &target).ok());
  EXPECT_EQ(target, "target");
  EXPECT_TRUE(meta.PrepareReclaim(99999999).ok());
}

}  // namespace swordfs::test::meta_contract
