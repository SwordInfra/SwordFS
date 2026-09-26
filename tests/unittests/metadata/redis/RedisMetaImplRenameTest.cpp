// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <thread>

#include "metadata/redis/RedisMetaImplTestBase.hpp"

namespace {

using swordfs::test::redis_meta::InodeID;
using swordfs::test::redis_meta::kRootInodeId;
using swordfs::test::redis_meta::kTestChunkSize;
using swordfs::test::redis_meta::MakePendingDelete;
using swordfs::test::redis_meta::PendingDeleteObjectKey;
using swordfs::test::redis_meta::ReclaimWork;
using swordfs::test::redis_meta::RedisMetaImpl;
using swordfs::test::redis_meta::RedisMetaImplTest;
using swordfs::test::redis_meta::SetAttrField;
using swordfs::test::redis_meta::Status;
using swordfs::test::redis_meta::SwordFsAttr;
using swordfs::test::redis_meta::SwordFsChunk;
using swordfs::test::redis_meta::SwordFsEntry;
using swordfs::test::redis_meta::SwordFsInode;
using swordfs::test::redis_meta::SwordFsVolume;

FIBER_TEST_F(RedisMetaImplTest, RenameDirectoryOverEmptyDirectoryUpdatesSameParentNlink) {
  SwordFsInode src;
  SwordFsInode dst;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "src", 0777, &src).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dst", 0777, &dst).ok());

  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  ASSERT_EQ(root.attr.nlink, 4U);

  ASSERT_TRUE(impl_->Rename(kRootInodeId, "src", kRootInodeId, "dst", swordfs::metadata::RenameFlag::kNone).ok());

  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  EXPECT_EQ(root.attr.nlink, 3U);
}

FIBER_TEST_F(RedisMetaImplTest, RenameDirectoryOverEmptyDirectoryUpdatesCrossParentNlink) {
  SwordFsInode dir_a;
  SwordFsInode dir_b;
  SwordFsInode src;
  SwordFsInode dst;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "a", 0777, &dir_a).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "b", 0777, &dir_b).ok());
  ASSERT_TRUE(impl_->MkDir(dir_a.ino, "src", 0777, &src).ok());
  ASSERT_TRUE(impl_->MkDir(dir_b.ino, "dst", 0777, &dst).ok());

  ASSERT_TRUE(impl_->Rename(dir_a.ino, "src", dir_b.ino, "dst", swordfs::metadata::RenameFlag::kNone).ok());

  SwordFsInode old_parent;
  SwordFsInode new_parent;
  ASSERT_TRUE(impl_->GetInode(dir_a.ino, &old_parent).ok());
  ASSERT_TRUE(impl_->GetInode(dir_b.ino, &new_parent).ok());
  // The old parent loses the moved directory's ".." backlink; the new parent
  // loses the victim's backlink and gains the moved directory's, net zero.
  EXPECT_EQ(old_parent.attr.nlink, 2U);
  EXPECT_EQ(new_parent.attr.nlink, 3U);
}

FIBER_TEST_F(RedisMetaImplTest, RenameCoversMoveOverwriteNoReplaceAndExchange) {
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "second", 0644, &second).ok());

  EXPECT_TRUE(impl_->Rename(kRootInodeId, ".", kRootInodeId, "x", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
              EBUSY);
  const std::string long_name(256, 'x');
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "first", kRootInodeId, long_name, swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      ENAMETOOLONG);
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", swordfs::metadata::RenameFlag::kNoReplace)
                  .ToErrno() == EEXIST);
  EXPECT_EQ(impl_
                ->Rename(kRootInodeId, "first", kRootInodeId, "second",
                         swordfs::metadata::RenameFlag::kNoReplace | swordfs::metadata::RenameFlag::kExchange)
                .ToErrno(),
            EINVAL);
  EXPECT_EQ(
      impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", static_cast<swordfs::metadata::RenameFlag>(1u << 7))
          .ToErrno(),
      EINVAL);

  ASSERT_TRUE(impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", swordfs::metadata::RenameFlag::kNone).ok());
  std::vector<InodeID> overwrite_orphans;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&](InodeID ino) {
                    overwrite_orphans.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(overwrite_orphans, std::vector<InodeID>{second.ino});

  SwordFsInode third;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "third", 0644, &third).ok());
  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "second", kRootInodeId, "third", swordfs::metadata::RenameFlag::kExchange).ok());
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "second", &found).ok());
  EXPECT_EQ(found.ino, third.ino);
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "third", &found).ok());
  EXPECT_EQ(found.ino, first.ino);

  EXPECT_TRUE(impl_->Rename(kRootInodeId, "third", kRootInodeId, "missing", swordfs::metadata::RenameFlag::kExchange)
                  .IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, RenameRejectsOrdinaryTypeMismatchAndDirectoryCycles) {
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "dir", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      EISDIR);
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      ENOTDIR);

  SwordFsInode child;
  ASSERT_TRUE(impl_->MkDir(dir.ino, "child", 0755, &child).ok());
  EXPECT_EQ(impl_->Rename(kRootInodeId, "dir", child.ino, "moved", swordfs::metadata::RenameFlag::kNone).ToErrno(),
            EINVAL);
}

FIBER_TEST_F(RedisMetaImplTest, RenameExchangeDirectoryAndFileSupportsBothDirections) {
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  ASSERT_TRUE(impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kExchange).ok());
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "dir", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "file", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);

  ASSERT_TRUE(impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kExchange).ok());
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
}

FIBER_TEST_F(RedisMetaImplTest, RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology) {
  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "left", 0755, &left).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "right", 0755, &right).ok());
  ASSERT_TRUE(impl_->MkDir(left.ino, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(right.ino, "file", 0644, &file).ok());

  SwordFsInode left_before;
  SwordFsInode right_before;
  ASSERT_TRUE(impl_->GetInode(left.ino, &left_before).ok());
  ASSERT_TRUE(impl_->GetInode(right.ino, &right_before).ok());

  ASSERT_TRUE(impl_->Rename(left.ino, "dir", right.ino, "file", swordfs::metadata::RenameFlag::kExchange).ok());

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(left.ino, "dir", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  ASSERT_TRUE(impl_->Lookup(right.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  EXPECT_EQ(found.parent_ino, right.ino);

  SwordFsInode left_after;
  SwordFsInode right_after;
  ASSERT_TRUE(impl_->GetInode(left.ino, &left_after).ok());
  ASSERT_TRUE(impl_->GetInode(right.ino, &right_after).ok());
  EXPECT_EQ(left_after.attr.nlink, left_before.attr.nlink - 1);
  EXPECT_EQ(right_after.attr.nlink, right_before.attr.nlink + 1);

  ASSERT_TRUE(impl_->Rename(left.ino, "dir", right.ino, "file", swordfs::metadata::RenameFlag::kExchange).ok());
  ASSERT_TRUE(impl_->Lookup(left.ino, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  EXPECT_EQ(found.parent_ino, left.ino);

  SwordFsInode left_restored;
  SwordFsInode right_restored;
  ASSERT_TRUE(impl_->GetInode(left.ino, &left_restored).ok());
  ASSERT_TRUE(impl_->GetInode(right.ino, &right_restored).ok());
  EXPECT_EQ(left_restored.attr.nlink, left_before.attr.nlink);
  EXPECT_EQ(right_restored.attr.nlink, right_before.attr.nlink);
}

FIBER_TEST_F(RedisMetaImplTest, RenameExchangeRejectsCycleWhenOnlySourceIsDirectory) {
  SwordFsInode dir;
  SwordFsInode descendant;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->MkDir(dir.ino, "descendant", 0755, &descendant).ok());
  ASSERT_TRUE(impl_->Create(descendant.ino, "file", 0644, &file).ok());

  EXPECT_EQ(
      impl_->Rename(kRootInodeId, "dir", descendant.ino, "file", swordfs::metadata::RenameFlag::kExchange).ToErrno(),
      EINVAL);
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  ASSERT_TRUE(impl_->Lookup(descendant.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
}

FIBER_TEST_F(RedisMetaImplTest, RenameExchangeRejectsCycleWhenOnlyTargetIsDirectory) {
  SwordFsInode dir;
  SwordFsInode descendant;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->MkDir(dir.ino, "descendant", 0755, &descendant).ok());
  ASSERT_TRUE(impl_->Create(descendant.ino, "file", 0644, &file).ok());

  EXPECT_EQ(
      impl_->Rename(descendant.ino, "file", kRootInodeId, "dir", swordfs::metadata::RenameFlag::kExchange).ToErrno(),
      EINVAL);
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(descendant.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
}

FIBER_TEST_F(RedisMetaImplTest, RenameExchangeNlinkFailureDoesNotPartiallyCommitNamespace) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto set_persisted_nlink = [&](InodeID ino, uint64_t nlink) {
    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      auto encoded = redis.get(key.Inode(ino));
      ASSERT_TRUE(encoded.has_value());
      SwordFsInode inode;
      ASSERT_TRUE(inode.ParseFrom(*encoded).ok());
      inode.attr.nlink = nlink;
      std::string updated;
      ASSERT_TRUE(inode.SerializeTo(&updated).ok());
      redis.set(key.Inode(ino), updated);
    });
  };

  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "left", 0755, &left).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "right", 0755, &right).ok());
  ASSERT_TRUE(impl_->MkDir(left.ino, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(right.ino, "file", 0644, &file).ok());

  set_persisted_nlink(left.ino, 0);
  EXPECT_EQ(impl_->Rename(left.ino, "dir", right.ino, "file", swordfs::metadata::RenameFlag::kExchange).ToErrno(), EIO);
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(left.ino, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  ASSERT_TRUE(impl_->Lookup(right.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);

  SwordFsInode file_parent;
  SwordFsInode dir_parent;
  SwordFsInode file2;
  SwordFsInode dir2;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "file-parent", 0755, &file_parent).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir-parent", 0755, &dir_parent).ok());
  ASSERT_TRUE(impl_->Create(file_parent.ino, "file", 0644, &file2).ok());
  ASSERT_TRUE(impl_->MkDir(dir_parent.ino, "dir", 0755, &dir2).ok());

  SwordFsInode file_parent_before;
  ASSERT_TRUE(impl_->GetInode(file_parent.ino, &file_parent_before).ok());
  set_persisted_nlink(dir_parent.ino, 0);
  EXPECT_EQ(
      impl_->Rename(file_parent.ino, "file", dir_parent.ino, "dir", swordfs::metadata::RenameFlag::kExchange).ToErrno(),
      EIO);
  ASSERT_TRUE(impl_->Lookup(file_parent.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file2.ino);
  ASSERT_TRUE(impl_->Lookup(dir_parent.ino, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir2.ino);

  SwordFsInode file_parent_after;
  ASSERT_TRUE(impl_->GetInode(file_parent.ino, &file_parent_after).ok());
  EXPECT_EQ(file_parent_after.attr.nlink, file_parent_before.attr.nlink);
}

FIBER_TEST_F(RedisMetaImplTest, RenameExchangeDirectoriesAcrossParentsUpdatesParents) {
  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode left_dir;
  SwordFsInode right_dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "left", 0755, &left).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "right", 0755, &right).ok());
  ASSERT_TRUE(impl_->MkDir(left.ino, "a", 0755, &left_dir).ok());
  ASSERT_TRUE(impl_->MkDir(right.ino, "b", 0755, &right_dir).ok());

  ASSERT_TRUE(impl_->Rename(left.ino, "a", right.ino, "b", swordfs::metadata::RenameFlag::kExchange).ok());

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(left.ino, "a", &found).ok());
  EXPECT_EQ(found.ino, right_dir.ino);
  EXPECT_EQ(found.parent_ino, left.ino);
  ASSERT_TRUE(impl_->Lookup(right.ino, "b", &found).ok());
  EXPECT_EQ(found.ino, left_dir.ino);
  EXPECT_EQ(found.parent_ino, right.ino);
}

FIBER_TEST_F(RedisMetaImplTest, RenameSameInodeThroughHardLinkIsNoOp) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "alias", nullptr).ok());

  ASSERT_TRUE(impl_->Rename(kRootInodeId, "file", kRootInodeId, "alias", swordfs::metadata::RenameFlag::kNone).ok());
  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "alias", swordfs::metadata::RenameFlag::kExchange).ok());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedRenameTargetInodeIsRejected) {
  SwordFsInode source;
  SwordFsInode target;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "source", 0644, &source).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "target", 0644, &target).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(target.ino), "malformed"); });

  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      EIO);
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kExchange)
                  .ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, RenameRejectsNonEmptyTargetDirectory) {
  SwordFsInode source_parent;
  SwordFsInode source;
  SwordFsInode target_parent;
  SwordFsInode target;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "src-parent", 0777, &source_parent).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dst-parent", 0777, &target_parent).ok());
  ASSERT_TRUE(impl_->MkDir(source_parent.ino, "source", 0755, &source).ok());
  ASSERT_TRUE(impl_->MkDir(target_parent.ino, "target", 0755, &target).ok());
  SwordFsInode child;
  ASSERT_TRUE(impl_->Create(target.ino, "child", 0644, &child).ok());

  EXPECT_TRUE(
      impl_->Rename(source_parent.ino, "source", target_parent.ino, "target", swordfs::metadata::RenameFlag::kNone)
          .ToErrno() == ENOTEMPTY);
}
}  // namespace
