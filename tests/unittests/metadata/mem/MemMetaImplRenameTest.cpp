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

class TestMemMetaImpl : public MemMetaImpl {
 public:
  using MemMetaImpl::Create;
  using MemMetaImpl::Lookup;
  using MemMetaImpl::MkDir;

  Status Create(InodeID parent_ino, std::string_view name, mode_t mode,
                InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::Create(parent_ino, name, mode, (ino || attr) ? &out : nullptr);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) *attr = out.attr;
    }
    return status;
  }
  Status MkDir(InodeID parent_ino, std::string_view name, mode_t mode,
               InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::MkDir(parent_ino, name, mode, (ino || attr) ? &out : nullptr);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) *attr = out.attr;
    }
    return status;
  }
  Status Lookup(InodeID parent_ino, std::string_view name,
                InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::Lookup(parent_ino, name, &out);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) *attr = out.attr;
    }
    return status;
  }
  Status GetAttr(InodeID ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::GetInode(ino, attr ? &out : nullptr);
    if (status.ok() && attr) *attr = out.attr;
    return status;
  }
};

class MemMetaImplRenameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new TestMemMetaImpl();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override { delete impl_; }

  TestMemMetaImpl *impl_;
};

// ════════════════════════════════════════════════════════════════════
// Basic Rename
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, BasicRenameFile) {
  InodeID f_ino = 0;
  impl_->Create(kRoot, "old_name", 0644, &f_ino, nullptr);

  Status st = impl_->Rename(kRoot, "old_name", kRoot, "new_name", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "old_name", &found, nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Lookup(kRoot, "new_name", &found, nullptr).ok());
  EXPECT_EQ(found, f_ino);
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
  InodeID sub_ino = 0;
  impl_->MkDir(kRoot, "sub", 0755, &sub_ino, nullptr);
  InodeID f_ino = 0;
  impl_->Create(sub_ino, "f", 0644, &f_ino, nullptr);

  Status st = impl_->Rename(sub_ino, "..", kRoot, "new", RenameFlag::kNone);
  EXPECT_TRUE(st.IsBusy()) << "should refuse to rename '..'";
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite existing file
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameOverwriteFileReportsVictim) {
  InodeID f1_ino = 0, f2_ino = 0;
  impl_->Create(kRoot, "src", 0644, &f1_ino, nullptr);
  impl_->Create(kRoot, "dst", 0644, &f2_ino, nullptr);

  RenameResult result;
  Status st = impl_->Rename(kRoot, "src", kRoot, "dst", RenameFlag::kNone,
                            &result);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(result.overwritten_ino, f2_ino);
  EXPECT_EQ(result.overwritten_post_nlink, 0);

  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "dst", &found, nullptr).ok());
  EXPECT_EQ(found, f1_ino);

  // The metadata transaction must not reclaim the overwritten file itself:
  // the VFS layer needs to decide whether an open handle still references it.
  struct stat attr;
  EXPECT_TRUE(impl_->GetAttr(f2_ino, &attr).ok());
  EXPECT_TRUE(impl_->ReclaimInode(f2_ino).ok());
  EXPECT_TRUE(impl_->GetAttr(f2_ino, &attr).IsNotFound());
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite empty directory
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameOverwriteEmptyDirectory) {
  InodeID dir1_ino = 0, dir2_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &dir1_ino, nullptr);
  impl_->MkDir(kRoot, "b", 0755, &dir2_ino, nullptr);

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
  InodeID src_ino = 0, dst_ino = 0;
  impl_->MkDir(kRoot, "src", 0755, &src_ino, nullptr);
  impl_->MkDir(kRoot, "dst", 0755, &dst_ino, nullptr);

  InodeID sub_ino = 0;
  impl_->MkDir(src_ino, "sub", 0755, &sub_ino, nullptr);

  struct stat src_attr_before, dst_attr_before;
  impl_->GetAttr(src_ino, &src_attr_before);
  impl_->GetAttr(dst_ino, &dst_attr_before);

  Status st = impl_->Rename(src_ino, "sub", dst_ino, "sub", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  struct stat src_attr_after, dst_attr_after;
  impl_->GetAttr(src_ino, &src_attr_after);
  impl_->GetAttr(dst_ino, &dst_attr_after);

  EXPECT_EQ(src_attr_after.st_nlink, src_attr_before.st_nlink - 1);
  EXPECT_EQ(dst_attr_after.st_nlink, dst_attr_before.st_nlink + 1);
}

// ════════════════════════════════════════════════════════════════════
// Rename: same-directory move of directory -> nlink unchanged
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameDirectorySameDirectoryNlinkUnchanged) {
  InodeID dir_ino = 0;
  impl_->MkDir(kRoot, "parent", 0755, &dir_ino, nullptr);
  InodeID sub_ino = 0;
  impl_->MkDir(dir_ino, "sub", 0755, &sub_ino, nullptr);

  struct stat parent_before;
  impl_->GetAttr(dir_ino, &parent_before);

  Status st = impl_->Rename(dir_ino, "sub", dir_ino, "renamed_sub", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  struct stat parent_after;
  impl_->GetAttr(dir_ino, &parent_after);
  EXPECT_EQ(parent_after.st_nlink, parent_before.st_nlink);
}

// ════════════════════════════════════════════════════════════════════
// Rename: cannot move directory into its own subtree
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoSubtreeFails) {
  InodeID a_ino = 0, b_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &a_ino, nullptr);
  impl_->MkDir(a_ino, "b", 0755, &b_ino, nullptr);

  Status st = impl_->Rename(kRoot, "a", b_ino, "a", RenameFlag::kNone);
  EXPECT_EQ(st.code(), Status::kInvalidArgument) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: file <-> directory type mismatch on overwrite
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameFileOverDirectoryFails) {
  InodeID f_ino = 0, d_ino = 0;
  impl_->Create(kRoot, "f", 0644, &f_ino, nullptr);
  impl_->MkDir(kRoot, "d", 0755, &d_ino, nullptr);

  Status st = impl_->Rename(kRoot, "f", kRoot, "d", RenameFlag::kNone);
  EXPECT_EQ(st.code(), Status::kIsDirectory) << st.message();
}

TEST_F(MemMetaImplRenameTest, RenameDirectoryOverFileFails) {
  InodeID f_ino = 0, d_ino = 0;
  impl_->Create(kRoot, "f", 0644, &f_ino, nullptr);
  impl_->MkDir(kRoot, "d", 0755, &d_ino, nullptr);

  Status st = impl_->Rename(kRoot, "d", kRoot, "f", RenameFlag::kNone);
  EXPECT_EQ(st.code(), Status::kNotDirectory) << st.message();
}

// ════════════════════════════════════════════════════════════════════
// Rename: overwrite non-empty directory fails
// ════════════════════════════════════════════════════════════════════

TEST_F(MemMetaImplRenameTest, RenameOverwriteNonEmptyDirectoryFails) {
  InodeID d1_ino = 0, d2_ino = 0;
  impl_->MkDir(kRoot, "d1", 0755, &d1_ino, nullptr);
  impl_->MkDir(kRoot, "d2", 0755, &d2_ino, nullptr);

  impl_->Create(d2_ino, "child", 0644, nullptr, nullptr);

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

  InodeID a_ino = 0, b_ino = 0, c_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &a_ino, nullptr);
  impl_->MkDir(kRoot, "b", 0755, &b_ino, nullptr);
  impl_->MkDir(kRoot, "c", 0755, &c_ino, nullptr);

  struct stat root_after_create;
  impl_->GetAttr(kRoot, &root_after_create);
  EXPECT_EQ(root_after_create.st_nlink, initial + 3);

  impl_->MkDir(a_ino, "a1", 0755, nullptr, nullptr);
  impl_->MkDir(b_ino, "b1", 0755, nullptr, nullptr);

  struct stat a_before, c_before;
  impl_->GetAttr(a_ino, &a_before);
  impl_->GetAttr(c_ino, &c_before);

  Status st = impl_->Rename(a_ino, "a1", c_ino, "a1", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();

  struct stat a_after, c_after;
  impl_->GetAttr(a_ino, &a_after);
  impl_->GetAttr(c_ino, &c_after);

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
  InodeID a_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &a_ino, nullptr);

  // Rename "a" to become a child of itself (mv a a/x).  The descendant
  // check alone misses this because IsDescendantOf(a, a) is false.
  Status status = impl_->Rename(kRoot, "a", a_ino, "x", RenameFlag::kNone);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // The directory must still be reachable from the root, unchanged.
  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found, nullptr).ok());
  EXPECT_EQ(found, a_ino);
  struct stat attr;
  ASSERT_TRUE(impl_->GetAttr(a_ino, &attr).ok());
  EXPECT_TRUE(S_ISDIR(attr.st_mode));
}

TEST_F(MemMetaImplRenameTest, RenameDirectoryIntoOwnSubtreeStillFails) {
  InodeID a_ino = 0, b_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &a_ino, nullptr);
  impl_->MkDir(a_ino, "b", 0755, &b_ino, nullptr);

  Status status = impl_->Rename(kRoot, "a", b_ino, "x", RenameFlag::kNone);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found, nullptr).ok());
  EXPECT_EQ(found, a_ino);
}

TEST_F(MemMetaImplRenameTest, RenameExchangeDirectoryIntoItselfFails) {
  InodeID a_ino = 0, b_ino = 0;
  impl_->MkDir(kRoot, "a", 0755, &a_ino, nullptr);
  impl_->MkDir(a_ino, "b", 0755, &b_ino, nullptr);

  // Exchange root/a with a/b: the moved directory's new parent would be
  // itself.  The EXCHANGE path runs the same cycle check.
  Status status =
      impl_->Rename(kRoot, "a", a_ino, "b", RenameFlag::kExchange);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // Both entries must be untouched.
  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "a", &found, nullptr).ok());
  EXPECT_EQ(found, a_ino);
  EXPECT_TRUE(impl_->Lookup(a_ino, "b", &found, nullptr).ok());
  EXPECT_EQ(found, b_ino);
}

TEST_F(MemMetaImplRenameTest, RenameExchangeWithAncestorDirectoryFails) {
  // Build root/b/x/a: dir b is an ancestor of dir a.
  InodeID b_ino = 0, x_ino = 0, a_ino = 0;
  impl_->MkDir(kRoot, "b", 0755, &b_ino, nullptr);
  impl_->MkDir(b_ino, "x", 0755, &x_ino, nullptr);
  impl_->MkDir(x_ino, "a", 0755, &a_ino, nullptr);

  // Exchange x/a with root/b: dir b would land inside its own subtree.
  // The source-side check (a into b) passes here — only the symmetric
  // check catches this direction.
  Status status =
      impl_->Rename(x_ino, "a", kRoot, "b", RenameFlag::kExchange);
  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();

  // The whole subtree must be untouched.
  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "b", &found, nullptr).ok());
  EXPECT_EQ(found, b_ino);
  EXPECT_TRUE(impl_->Lookup(b_ino, "x", &found, nullptr).ok());
  EXPECT_EQ(found, x_ino);
  EXPECT_TRUE(impl_->Lookup(x_ino, "a", &found, nullptr).ok());
  EXPECT_EQ(found, a_ino);
}
