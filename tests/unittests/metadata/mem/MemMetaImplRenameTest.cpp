// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Additional Rename tests for MemMetaImpl covering:
// - nlink counting for cross-directory directory moves
// - overwrite behaviors (file, empty directory)
// - RENAME_NOREPLACE / RENAME_EXCHANGE flag handling (PR #24)

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::RenameResult;
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
  void TearDown() override { delete impl_; }

  MemMetaImpl *impl_;
};

// ════════════════════════════════════════════════════════════════════
// Basic Rename
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, BasicRenameFile) {
  SwordFsInode f;
  impl_->Create(kRoot, "old_name", 0644, &f);

  Status st = impl_->Rename(kRoot, "old_name", kRoot, "new_name", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "old_name", &found).IsNotFound());
  EXPECT_TRUE(impl_->Lookup(kRoot, "new_name", &found).ok());
  EXPECT_EQ(found.ino, f.ino);
}

TEST_F(MemMetaImplRenameTest, RenameSourceNotFound) {
  Status st = impl_->Rename(kRoot, "no_such", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

TEST_F(MemMetaImplRenameTest, RenameRefusesDot) {
  Status st = impl_->Rename(kRoot, ".", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.IsBusy()) << "should refuse to rename '.'";
}

TEST_F(MemMetaImplRenameTest, RenameRefusesDotDot) {
  SwordFsInode sub;
  impl_->MkDir(kRoot, "sub", 0755, &sub);
  SwordFsInode f;
  impl_->Create(sub.ino, "f", 0644, &f);

  Status st = impl_->Rename(sub.ino, "..", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.IsBusy()) << "should refuse to rename '..'";
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite existing file
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameOverwriteFileReportsVictim) {
  SwordFsInode f1, f2;
  impl_->Create(kRoot, "src", 0644, &f1);
  impl_->Create(kRoot, "dst", 0644, &f2);

  RenameResult result;
  Status st = impl_->Rename(kRoot, "src", kRoot, "dst", RenameFlag::kNone,
                            &result);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(result.overwritten_ino, f2.ino);
  EXPECT_EQ(result.overwritten_post_nlink, 0);

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "dst", &found).ok());
  EXPECT_EQ(found.ino, f1.ino);

  // The metadata transaction must not reclaim the overwritten file itself:
  // the VFS layer needs to decide whether an open handle still references it.
  struct stat attr;
  EXPECT_TRUE(impl_->GetAttr(f2.ino, &attr).ok());
  EXPECT_TRUE(impl_->ReclaimInode(f2.ino).ok());
  EXPECT_TRUE(impl_->GetAttr(f2.ino, &attr).IsNotFound());
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite empty directory
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameOverwriteEmptyDirectory) {
  SwordFsInode dir1, dir2;
  impl_->MkDir(kRoot, "a", 0755, &dir1);
  impl_->MkDir(kRoot, "b", 0755, &dir2);

  struct stat root_attr;
  impl_->GetAttr(kRoot, &root_attr);
  nlink_t nlink_before = root_attr.st_nlink;

  Status st = impl_->Rename(kRoot, "a", kRoot, "b", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  impl_->GetAttr(kRoot, &root_attr);
  EXPECT_EQ(root_attr.st_nlink, nlink_before - 1);
}

// ════════════════════════════════════════════════════════════════════
// Rename: cross-directory directory move -> nlink adjustments
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameDirectoryCrossDirectoryUpdatesNlink) {
  SwordFsInode src, dst;
  impl_->MkDir(kRoot, "src", 0755, &src);
  impl_->MkDir(kRoot, "dst", 0755, &dst);

  SwordFsInode sub;
  impl_->MkDir(src.ino, "sub", 0755, &sub);

  struct stat src_attr_before, dst_attr_before;
  impl_->GetAttr(src.ino, &src_attr_before);
  impl_->GetAttr(dst.ino, &dst_attr_before);

  Status st = impl_->Rename(src.ino, "sub", dst.ino, "sub", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  struct stat src_attr_after, dst_attr_after;
  impl_->GetAttr(src.ino, &src_attr_after);
  impl_->GetAttr(dst.ino, &dst_attr_after);

  EXPECT_EQ(src_attr_after.st_nlink, src_attr_before.st_nlink - 1);
  EXPECT_EQ(dst_attr_after.st_nlink, dst_attr_before.st_nlink + 1);
}

// ════════════════════════════════════════════════════════════════════
// Rename: same-directory move of directory -> nlink unchanged
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameDirectorySameDirectoryNlinkUnchanged) {
  SwordFsInode dir;
  impl_->MkDir(kRoot, "parent", 0755, &dir);
  SwordFsInode sub;
  impl_->MkDir(dir.ino, "sub", 0755, &sub);

  struct stat parent_before;
  impl_->GetAttr(dir.ino, &parent_before);

  Status st = impl_->Rename(dir.ino, "sub", dir.ino, "renamed_sub", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  struct stat parent_after;
  impl_->GetAttr(dir.ino, &parent_after);
  EXPECT_EQ(parent_after.st_nlink, parent_before.st_nlink);
}

// ════════════════════════════════════════════════════════════════════
// Rename: cannot move directory into its own subtree
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoSubtreeFails) {
  SwordFsInode a, b;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(a.ino, "b", 0755, &b);

  Status st = impl_->Rename(kRoot, "a", b.ino, "a", RenameFlag::kNone);
  EXPECT_EQ(st.code(), Status::kInvalidArgument) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: file <-> directory type mismatch on overwrite
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameFileOverDirectoryFails) {
  SwordFsInode f, d;
  impl_->Create(kRoot, "f", 0644, &f);
  impl_->MkDir(kRoot, "d", 0755, &d);

  Status st = impl_->Rename(kRoot, "f", kRoot, "d", RenameFlag::kNone);
  EXPECT_EQ(st.code(), Status::kIsDirectory) << st.message();
}

TEST_F(MemMetaImplRenameTest, RenameDirectoryOverFileFails) {
  SwordFsInode f, d;
  impl_->Create(kRoot, "f", 0644, &f);
  impl_->MkDir(kRoot, "d", 0755, &d);

  Status st = impl_->Rename(kRoot, "d", kRoot, "f", RenameFlag::kNone);
  EXPECT_EQ(st.code(), Status::kNotDirectory) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite non-empty directory fails
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameOverwriteNonEmptyDirectoryFails) {
  SwordFsInode d1, d2;
  impl_->MkDir(kRoot, "d1", 0755, &d1);
  impl_->MkDir(kRoot, "d2", 0755, &d2);

  impl_->Create(d2.ino, "child", 0644, nullptr);

  Status st = impl_->Rename(kRoot, "d1", kRoot, "d2", RenameFlag::kNone);
  EXPECT_TRUE(st.IsNotEmpty()) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: nlink accounting with multiple directories
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, NlinkAccountingMultipleDirs) {
  struct stat root_before;
  impl_->GetAttr(kRoot, &root_before);
  nlink_t initial = root_before.st_nlink;

  SwordFsInode a, b, c;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(kRoot, "b", 0755, &b);
  impl_->MkDir(kRoot, "c", 0755, &c);

  struct stat root_after_create;
  impl_->GetAttr(kRoot, &root_after_create);
  EXPECT_EQ(root_after_create.st_nlink, initial + 3);

  impl_->MkDir(a.ino, "a1", 0755, nullptr);
  impl_->MkDir(b.ino, "b1", 0755, nullptr);

  struct stat a_before, c_before;
  impl_->GetAttr(a.ino, &a_before);
  impl_->GetAttr(c.ino, &c_before);

  Status st = impl_->Rename(a.ino, "a1", c.ino, "a1", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  struct stat a_after, c_after;
  impl_->GetAttr(a.ino, &a_after);
  impl_->GetAttr(c.ino, &c_after);

  EXPECT_EQ(a_after.st_nlink, a_before.st_nlink - 1);
  EXPECT_EQ(c_after.st_nlink, c_before.st_nlink + 1);

  struct stat root_final;
  impl_->GetAttr(kRoot, &root_final);
  EXPECT_EQ(root_final.st_nlink, root_after_create.st_nlink);
}

// ════════════════════════════════════════════════════════════════════
// Rename: directory into itself (META-01)
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoItselfFails) {
  SwordFsInode a;
  impl_->MkDir(kRoot, "a", 0755, &a);

  // Rename "a" to become a child of itself (mv a a/x).  The descendant
  // check alone misses this because IsDescendantOf(a, a) is false.
  Status status = impl_->Rename(kRoot, "a", a.ino, "x", RenameFlag::kNone);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // The directory must still be reachable from the root, unchanged.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
  struct stat attr;
  ASSERT_TRUE(impl_->GetAttr(a.ino, &attr).ok());
  EXPECT_TRUE(S_ISDIR(attr.st_mode));
}

TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoOwnSubtreeStillFails) {
  SwordFsInode a, b;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(a.ino, "b", 0755, &b);

  Status status = impl_->Rename(kRoot, "a", b.ino, "x", RenameFlag::kNone);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
}

TEST_F(MemMetaImplRenameTest, RenameExchangeDirectoryIntoItselfFails) {
  SwordFsInode a, b;
  impl_->MkDir(kRoot, "a", 0755, &a);
  impl_->MkDir(a.ino, "b", 0755, &b);

  // Exchange root/a with a/b: the moved directory's new parent would be
  // itself.  The EXCHANGE path runs the same cycle check.
  Status status =
      impl_->Rename(kRoot, "a", a.ino, "b", RenameFlag::kExchange);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // Both entries must be untouched.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
  EXPECT_TRUE(impl_->Lookup(a.ino, "b", &found).ok());
  EXPECT_EQ(found.ino, b.ino);
}

TEST_F(MemMetaImplRenameTest, RenameExchangeWithAncestorDirectoryFails) {
  // Build root/b/x/a: dir b is an ancestor of dir a.
  SwordFsInode b, x, a;
  impl_->MkDir(kRoot, "b", 0755, &b);
  impl_->MkDir(b.ino, "x", 0755, &x);
  impl_->MkDir(x.ino, "a", 0755, &a);

  // Exchange x/a with root/b: dir b would land inside its own subtree.
  // The source-side check (a into b) passes here — only the symmetric
  // check catches this direction.
  Status status =
      impl_->Rename(x.ino, "a", kRoot, "b", RenameFlag::kExchange);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // The whole subtree must be untouched.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "b", &found).ok());
  EXPECT_EQ(found.ino, b.ino);
  EXPECT_TRUE(impl_->Lookup(b.ino, "x", &found).ok());
  EXPECT_EQ(found.ino, x.ino);
  EXPECT_TRUE(impl_->Lookup(x.ino, "a", &found).ok());
  EXPECT_EQ(found.ino, a.ino);
}
