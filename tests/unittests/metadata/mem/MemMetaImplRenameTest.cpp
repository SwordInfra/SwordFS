// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Additional Rename tests for MemMetaImpl covering:
// - nlink counting for cross-directory directory moves
// - overwrite behaviors (file, empty directory)
// - RENAME_NOREPLACE / RENAME_EXCHANGE flag handling (PR #24)

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>

#include "FiberTest.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;

class MemMetaImplRenameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new MemMetaImpl();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override {
    delete impl_;
  }

  MemMetaImpl *impl_;
};

// ════════════════════════════════════════════════════════════════════
// Basic Rename
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, BasicRenameFile) {
  SwordFsInode file;
  impl_->Create(kRoot, "old_name", 0644, &file);

  Status st = impl_->Rename(kRoot, "old_name", kRoot, "new_name", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "old_name", &found).IsNotFound());
  EXPECT_TRUE(impl_->Lookup(kRoot, "new_name", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameSourceNotFound) {
  Status st = impl_->Rename(kRoot, "no_such", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameRefusesDot) {
  Status st = impl_->Rename(kRoot, ".", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.ToErrno() == EBUSY) << "should refuse to rename '.'";
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameRefusesDotDot) {
  SwordFsInode sub;
  impl_->MkDir(kRoot, "sub", 0755, &sub);
  impl_->Create(sub.ino, "f", 0644, nullptr);

  Status st = impl_->Rename(sub.ino, "..", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.ToErrno() == EBUSY) << "should refuse to rename '..'";
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite existing file
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameOverwriteFilePublishesVictimForReclaim) {
  SwordFsInode src;
  SwordFsInode dst;
  impl_->Create(kRoot, "src", 0644, &src);
  impl_->Create(kRoot, "dst", 0644, &dst);

  Status st = impl_->Rename(kRoot, "src", kRoot, "dst", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "dst", &found).ok());
  EXPECT_EQ(found.ino, src.ino);

  // The metadata transaction must not reclaim the overwritten file itself:
  // the VFS layer needs to decide whether an open handle still references it.
  SwordFsInode victim;
  EXPECT_TRUE(impl_->GetInode(dst.ino, &victim).ok());
  std::optional<swordfs::metadata::ReclaimWork> reclaim_work;
  ASSERT_TRUE(impl_->PrepareReclaim(dst.ino, &reclaim_work).ok());
  ASSERT_TRUE(reclaim_work.has_value());
  ASSERT_TRUE(impl_->CompleteReclaim(dst.ino).ok());
  EXPECT_TRUE(impl_->GetInode(dst.ino, &victim).IsNotFound());
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite empty directory
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameOverwriteEmptyDirectory) {
  impl_->MkDir(kRoot, "a", 0755, nullptr);
  impl_->MkDir(kRoot, "b", 0755, nullptr);

  SwordFsInode root_before;
  impl_->GetInode(kRoot, &root_before);
  const uint64_t nlink_before = root_before.attr.nlink;

  Status st = impl_->Rename(kRoot, "a", kRoot, "b", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode root_after;
  impl_->GetInode(kRoot, &root_after);
  EXPECT_EQ(root_after.attr.nlink, nlink_before - 1);
}

// ════════════════════════════════════════════════════════════════════
// Rename: cross-directory directory move -> nlink adjustments
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameDirectoryCrossDirectoryUpdatesNlink) {
  SwordFsInode src;
  SwordFsInode dst;
  impl_->MkDir(kRoot, "src", 0755, &src);
  impl_->MkDir(kRoot, "dst", 0755, &dst);

  impl_->MkDir(src.ino, "sub", 0755, nullptr);

  SwordFsInode src_before;
  SwordFsInode dst_before;
  impl_->GetInode(src.ino, &src_before);
  impl_->GetInode(dst.ino, &dst_before);

  Status st = impl_->Rename(src.ino, "sub", dst.ino, "sub", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode src_after;
  SwordFsInode dst_after;
  impl_->GetInode(src.ino, &src_after);
  impl_->GetInode(dst.ino, &dst_after);

  EXPECT_EQ(src_after.attr.nlink, src_before.attr.nlink - 1);
  EXPECT_EQ(dst_after.attr.nlink, dst_before.attr.nlink + 1);
}

// ════════════════════════════════════════════════════════════════════
// Rename: same-directory move of directory -> nlink unchanged
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameDirectorySameDirectoryNlinkUnchanged) {
  SwordFsInode parent;
  impl_->MkDir(kRoot, "parent", 0755, &parent);
  impl_->MkDir(parent.ino, "sub", 0755, nullptr);

  SwordFsInode parent_before;
  impl_->GetInode(parent.ino, &parent_before);

  Status st = impl_->Rename(parent.ino, "sub", parent.ino, "renamed_sub", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode parent_after;
  impl_->GetInode(parent.ino, &parent_after);
  EXPECT_EQ(parent_after.attr.nlink, parent_before.attr.nlink);
}

// ════════════════════════════════════════════════════════════════════
// Rename: cannot move directory into its own subtree
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoSubtreeFails) {
  SwordFsInode a;
  SwordFsInode b;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(a.ino, "b", 0755, &b);

  Status st = impl_->Rename(kRoot, "a", b.ino, "a", RenameFlag::kNone);
  EXPECT_EQ(st.ToErrno(), EINVAL) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: file <-> directory type mismatch on overwrite
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameFileOverDirectoryFails) {
  impl_->Create(kRoot, "f", 0644, nullptr);
  impl_->MkDir(kRoot, "d", 0755, nullptr);

  Status st = impl_->Rename(kRoot, "f", kRoot, "d", RenameFlag::kNone);
  EXPECT_EQ(st.ToErrno(), EISDIR) << st.message();
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameDirectoryOverFileFails) {
  impl_->Create(kRoot, "f", 0644, nullptr);
  impl_->MkDir(kRoot, "d", 0755, nullptr);

  Status st = impl_->Rename(kRoot, "d", kRoot, "f", RenameFlag::kNone);
  EXPECT_EQ(st.ToErrno(), ENOTDIR) << st.message();
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeFileAndDirectorySameParentSucceeds) {
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(kRoot, "d", 0755, &dir).ok());

  Status status = impl_->Rename(kRoot, "f", kRoot, "d", RenameFlag::kExchange);
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRoot, "f", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  ASSERT_TRUE(impl_->Lookup(kRoot, "d", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeDirectoryAndFileAcrossParentsUpdatesTopology) {
  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRoot, "left", 0755, &left).ok());
  ASSERT_TRUE(impl_->MkDir(kRoot, "right", 0755, &right).ok());
  ASSERT_TRUE(impl_->MkDir(left.ino, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(right.ino, "file", 0644, &file).ok());

  SwordFsInode left_before;
  SwordFsInode right_before;
  ASSERT_TRUE(impl_->GetInode(left.ino, &left_before).ok());
  ASSERT_TRUE(impl_->GetInode(right.ino, &right_before).ok());

  Status status = impl_->Rename(left.ino, "dir", right.ino, "file", RenameFlag::kExchange);
  ASSERT_TRUE(status.ok()) << status.message();

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

  status = impl_->Rename(left.ino, "dir", right.ino, "file", RenameFlag::kExchange);
  ASSERT_TRUE(status.ok()) << status.message();
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

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeRejectsCycleWhenSourceOnlyIsDirectory) {
  SwordFsInode dir;
  SwordFsInode descendant;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRoot, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->MkDir(dir.ino, "descendant", 0755, &descendant).ok());
  ASSERT_TRUE(impl_->Create(descendant.ino, "file", 0644, &file).ok());

  Status status = impl_->Rename(kRoot, "dir", descendant.ino, "file", RenameFlag::kExchange);
  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRoot, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  ASSERT_TRUE(impl_->Lookup(descendant.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeRejectsCycleWhenTargetOnlyIsDirectory) {
  SwordFsInode dir;
  SwordFsInode descendant;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRoot, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->MkDir(dir.ino, "descendant", 0755, &descendant).ok());
  ASSERT_TRUE(impl_->Create(descendant.ino, "file", 0644, &file).ok());

  Status status = impl_->Rename(descendant.ino, "file", kRoot, "dir", RenameFlag::kExchange);
  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(descendant.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  ASSERT_TRUE(impl_->Lookup(kRoot, "dir", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeSameInodeAcrossParentsIsNoOp) {
  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRoot, "left", 0755, &left).ok());
  ASSERT_TRUE(impl_->MkDir(kRoot, "right", 0755, &right).ok());
  ASSERT_TRUE(impl_->Create(left.ino, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Link(file.ino, right.ino, "alias", nullptr).ok());

  SwordFsInode before;
  ASSERT_TRUE(impl_->GetInode(file.ino, &before).ok());
  ASSERT_EQ(before.parent_ino, left.ino);

  Status status = impl_->Rename(right.ino, "alias", left.ino, "file", RenameFlag::kExchange);
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(right.ino, "alias", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  ASSERT_TRUE(impl_->Lookup(left.ino, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);

  SwordFsInode after;
  ASSERT_TRUE(impl_->GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.parent_ino, before.parent_ino);
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite non-empty directory fails
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameOverwriteNonEmptyDirectoryFails) {
  SwordFsInode d1;
  SwordFsInode d2;
  impl_->MkDir(kRoot, "d1", 0755, &d1);
  impl_->MkDir(kRoot, "d2", 0755, &d2);

  impl_->Create(d2.ino, "child", 0644, nullptr);

  Status st = impl_->Rename(kRoot, "d1", kRoot, "d2", RenameFlag::kNone);
  EXPECT_TRUE(st.ToErrno() == ENOTEMPTY) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: nlink accounting with multiple directories
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, NlinkAccountingMultipleDirs) {
  SwordFsInode root_before;
  impl_->GetInode(kRoot, &root_before);
  const uint64_t initial = root_before.attr.nlink;

  SwordFsInode a;
  SwordFsInode b;
  SwordFsInode c;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(kRoot, "b", 0755, &b);
  impl_->MkDir(kRoot, "c", 0755, &c);

  SwordFsInode root_after_create;
  impl_->GetInode(kRoot, &root_after_create);
  EXPECT_EQ(root_after_create.attr.nlink, initial + 3);

  impl_->MkDir(a.ino, "a1", 0755, nullptr);
  impl_->MkDir(b.ino, "b1", 0755, nullptr);

  SwordFsInode a_before;
  SwordFsInode c_before;
  impl_->GetInode(a.ino, &a_before);
  impl_->GetInode(c.ino, &c_before);

  Status st = impl_->Rename(a.ino, "a1", c.ino, "a1", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode a_after;
  SwordFsInode c_after;
  impl_->GetInode(a.ino, &a_after);
  impl_->GetInode(c.ino, &c_after);

  EXPECT_EQ(a_after.attr.nlink, a_before.attr.nlink - 1);
  EXPECT_EQ(c_after.attr.nlink, c_before.attr.nlink + 1);

  SwordFsInode root_final;
  impl_->GetInode(kRoot, &root_final);
  EXPECT_EQ(root_final.attr.nlink, root_after_create.attr.nlink);
}

// ════════════════════════════════════════════════════════════════════
// Rename: directory into itself (META-01)
// ════════════════════════════════════════════════════════════════════

FIBER_TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoItselfFails) {
  SwordFsInode a;
  impl_->MkDir(kRoot, "a", 0755, &a);

  // Rename "a" to become a child of itself (mv a a/x).  The descendant
  // check alone misses this because IsDescendantOf(a, a) is false.
  Status status = impl_->Rename(kRoot, "a", a.ino, "x", RenameFlag::kNone);
  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();

  // The directory must still be reachable from the root, unchanged.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
  ASSERT_TRUE(impl_->GetInode(a.ino, &found).ok());
  EXPECT_TRUE(S_ISDIR(found.attr.mode));
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoOwnSubtreeStillFails) {
  SwordFsInode a;
  SwordFsInode b;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(a.ino, "b", 0755, &b);

  Status status = impl_->Rename(kRoot, "a", b.ino, "x", RenameFlag::kNone);
  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeDirectoryIntoItselfFails) {
  SwordFsInode a;
  SwordFsInode b;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(a.ino, "b", 0755, &b);

  // Exchange root/a with a/b: the moved directory's new parent would be
  // itself.  The EXCHANGE path runs the same cycle check.
  Status status = impl_->Rename(kRoot, "a", a.ino, "b", RenameFlag::kExchange);
  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();

  // Both entries must be untouched.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
  EXPECT_TRUE(impl_->Lookup(a.ino, "b", &found).ok());
  EXPECT_EQ(found.ino, b.ino);
}

FIBER_TEST_F(MemMetaImplRenameTest, RenameExchangeWithAncestorDirectoryFails) {
  // Build root/b/x/a: dir b is an ancestor of dir a.
  SwordFsInode b;
  SwordFsInode x;
  SwordFsInode a;
  impl_->MkDir(kRoot, "b", 0755, &b);
  impl_->MkDir(b.ino, "x", 0755, &x);
  impl_->MkDir(x.ino, "a", 0755, &a);

  // Exchange x/a with root/b: dir b would land inside its own subtree.
  // The source-side check (a into b) passes here — only the symmetric
  // check catches this direction.
  Status status = impl_->Rename(x.ino, "a", kRoot, "b", RenameFlag::kExchange);
  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();

  // The whole subtree must be untouched.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "b", &found).ok());
  EXPECT_EQ(found.ino, b.ino);
  EXPECT_TRUE(impl_->Lookup(b.ino, "x", &found).ok());
  EXPECT_EQ(found.ino, x.ino);
  EXPECT_TRUE(impl_->Lookup(x.ino, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
}
