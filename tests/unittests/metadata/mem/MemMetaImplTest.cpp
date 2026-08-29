// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaImpl: permission checks, open-unlink behaviour, and
// operation-level atomicity under concurrency.

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <barrier>
#include <thread>

#include "TestMemMetaImpl.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::test::TestMemMetaImpl;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
static constexpr uid_t kOwner = 1000;
static constexpr uid_t kOther = 2000;
static constexpr gid_t kGroup = 100;
static constexpr gid_t kOtherGroup = 200;

class MemMetaImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new TestMemMetaImpl();
    // Default context is root (uid=0, gid=0).
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override {
    delete impl_;
  }

  // Set the fiber-local context for the current thread.
  void SetContext(uid_t uid, gid_t gid) {
    auto &ctx = folly::fibers::local<SwordFsContext>();
    ctx.uid = uid;
    ctx.gid = gid;
  }

  // ────────────────────────────────────────────────────────────────
  // Helpers to set up directories with specific ownership & perms
  // ────────────────────────────────────────────────────────────────

  // Create a directory owned by kOwner:kGroup with the given mode.
  // Must be called while context is root.
  InodeID MakeOwnedDir(InodeID parent_ino, std::string_view name, mode_t mode) {
    SetContext(0, 0);  // root
    SwordFsInode inode;
    impl_->MkDir(parent_ino, name, mode, &inode);
    InodeID ino = inode.ino;
    // Change ownership to kOwner:kGroup
    struct stat st {};
    st.st_uid = kOwner;
    st.st_gid = kGroup;
    st.st_mode = S_IFDIR | mode;
    impl_->SetAttr(ino, &st, SetAttrField::kUid | SetAttrField::kGid | SetAttrField::kMode, nullptr);
    return ino;
  }

  // Change only the mode of an existing directory.
  void SetDirMode(InodeID ino, mode_t mode) {
    SetContext(0, 0);
    struct stat st {};
    st.st_mode = S_IFDIR | mode;
    impl_->SetAttr(ino, &st, SetAttrField::kMode, nullptr);
  }

  Status CreateFile(InodeID parent_ino, std::string_view name, mode_t mode, InodeID *ino = nullptr) {
    SwordFsInode inode;
    Status status = impl_->Create(parent_ino, name, mode, ino ? &inode : nullptr);
    if (status.ok() && ino) {
      *ino = inode.ino;
    }
    return status;
  }

  Status MakeDir(InodeID parent_ino, std::string_view name, mode_t mode, InodeID *ino = nullptr) {
    SwordFsInode inode;
    Status status = impl_->MkDir(parent_ino, name, mode, ino ? &inode : nullptr);
    if (status.ok() && ino) {
      *ino = inode.ino;
    }
    return status;
  }

  Status LookupInode(InodeID parent_ino, std::string_view name, InodeID *ino) {
    SwordFsInode inode;
    Status status = impl_->Lookup(parent_ino, name, &inode);
    if (status.ok() && ino) {
      *ino = inode.ino;
    }
    return status;
  }

  Status GetInodeAttr(InodeID ino, struct stat *attr) {
    SwordFsInode inode;
    Status status = impl_->GetInode(ino, &inode);
    if (status.ok() && attr) {
      attr->st_ino = static_cast<ino_t>(inode.attr.ino);
      attr->st_mode = static_cast<mode_t>(inode.attr.mode);
      attr->st_nlink = static_cast<nlink_t>(inode.attr.nlink);
      attr->st_uid = static_cast<uid_t>(inode.attr.uid);
      attr->st_gid = static_cast<gid_t>(inode.attr.gid);
      attr->st_size = static_cast<off_t>(inode.attr.size);
      attr->st_blksize = static_cast<blksize_t>(inode.attr.blksize);
      attr->st_blocks = static_cast<blkcnt_t>(inode.attr.blocks);
      attr->st_atime = static_cast<time_t>(inode.attr.atime);
      attr->st_atim.tv_nsec = static_cast<long>(inode.attr.atime_nsec);
      attr->st_mtime = static_cast<time_t>(inode.attr.mtime);
      attr->st_mtim.tv_nsec = static_cast<long>(inode.attr.mtime_nsec);
      attr->st_ctime = static_cast<time_t>(inode.attr.ctime);
      attr->st_ctim.tv_nsec = static_cast<long>(inode.attr.ctime_nsec);
    }
    return status;
  }

  // Change ownership of an existing directory.
  void SetDirOwner(InodeID ino, uid_t uid, gid_t gid) {
    SetContext(0, 0);
    struct stat st {};
    st.st_uid = uid;
    st.st_gid = gid;
    impl_->SetAttr(ino, &st, SetAttrField::kUid | SetAttrField::kGid, nullptr);
  }

  TestMemMetaImpl *impl_;
};

// ────────────────────────────────────────────────────────────────
// Create permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, CreateOwnerWithWriteAndExecSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);

  SwordFsInode inode;
  Status st = impl_->Create(dir_ino, "f", 0644, &inode);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOwnerWithoutWriteFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0500);  // r-x, no write
  SetContext(kOwner, kOtherGroup);

  SwordFsInode inode;
  Status st = impl_->Create(dir_ino, "f", 0644, &inode);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOwnerWithoutExecFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0600);  // rw-, no exec
  SetContext(kOwner, kOtherGroup);

  SwordFsInode inode;
  Status st = impl_->Create(dir_ino, "f", 0644, &inode);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOwnerNoPermsFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);
  SetContext(kOwner, kOtherGroup);

  SwordFsInode inode;
  Status st = impl_->Create(dir_ino, "f", 0644, &inode);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateRootAlwaysSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);  // no perms at all
  SetContext(0, 0);                                  // root

  SwordFsInode inode;
  Status st = impl_->Create(dir_ino, "f", 0644, &inode);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateGroupMemberWithWriteExecSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0770);
  // kOther is NOT the owner, but IS in kGroup
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOther, kGroup);

  Status st = impl_->Create(dir_ino, "f", 0644, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateGroupMemberWithoutWriteFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0750);  // group has r-x only
  // Caller is in kGroup but is NOT the owner (kOwner=1000).
  SetContext(3000, kGroup);

  Status st = impl_->Create(dir_ino, "f", 0644, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOtherWithWriteExecSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0777);
  // Not owner, not in group
  SetDirOwner(dir_ino, kOwner, kGroup);
  SetContext(kOther, kOtherGroup);

  Status st = impl_->Create(dir_ino, "f", 0644, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOtherWithoutWriteFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0755);  // other has r-x only
  SetDirOwner(dir_ino, kOwner, kGroup);
  SetContext(kOther, kOtherGroup);

  Status st = impl_->Create(dir_ino, "f", 0644, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// MkDir permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, MkDirPermissionDeniedWithoutWrite) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0500);  // no write for owner
  SetContext(kOwner, kOtherGroup);

  Status st = impl_->MkDir(dir_ino, "sub", 0755, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, MkDirRootAlwaysSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);
  SetContext(0, 0);

  Status st = impl_->MkDir(dir_ino, "sub", 0755, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// Access permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, AccessOwnerPermissions) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);  // owner rwx
  SetContext(kOwner, kOtherGroup);

  EXPECT_TRUE(impl_->Access(dir_ino, R_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, W_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, X_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, R_OK | W_OK).ok());
}

TEST_F(MemMetaImplTest, AccessOwnerReadOnly) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0400);  // owner r--
  SetContext(kOwner, kOtherGroup);

  EXPECT_TRUE(impl_->Access(dir_ino, R_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, W_OK).IsPermission());
  EXPECT_TRUE(impl_->Access(dir_ino, X_OK).IsPermission());
}

TEST_F(MemMetaImplTest, AccessGroupPermissions) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0070);  // group rwx
  SetDirOwner(dir_ino, kOther, kGroup);
  // Caller is NOT the owner (kOther=2000), but IS in kGroup (100).
  SetContext(3000, kGroup);

  EXPECT_TRUE(impl_->Access(dir_ino, R_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, W_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, X_OK).ok());
}

TEST_F(MemMetaImplTest, AccessOtherPermissions) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0007);  // other rwx
  SetDirOwner(dir_ino, kOwner, kGroup);
  SetContext(kOther, kOtherGroup);

  EXPECT_TRUE(impl_->Access(dir_ino, R_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, W_OK).ok());
  EXPECT_TRUE(impl_->Access(dir_ino, X_OK).ok());
}

TEST_F(MemMetaImplTest, AccessRootAlwaysHasFullAccess) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);  // no perms
  SetContext(0, 0);

  EXPECT_TRUE(impl_->Access(dir_ino, R_OK | W_OK | X_OK).ok());
}

TEST_F(MemMetaImplTest, AccessNotFoundFails) {
  SetContext(kOwner, kGroup);
  Status st = impl_->Access(99999, R_OK);
  EXPECT_TRUE(st.IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// Unlink permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, UnlinkOwnerWithWriteSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  // Create a file owned by kOwner (the creator)
  SetContext(kOwner, kOtherGroup);
  impl_->Create(dir_ino, "f", 0644, nullptr);

  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

TEST_F(MemMetaImplTest, UnlinkWithoutWriteOnParentFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);
  impl_->Create(dir_ino, "f", 0644, nullptr);

  // Remove write from parent, keep exec
  SetDirMode(dir_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->Unlink(dir_ino, "f");
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// Unlink sticky-bit checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, UnlinkStickyBitOwnerCanDelete) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01700);  // sticky + rwx for owner
  SetContext(kOwner, kOtherGroup);
  impl_->Create(dir_ino, "f", 0644, nullptr);

  // The file's owner is kOwner (since kOwner created it).
  // kOwner is also the dir owner.
  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

TEST_F(MemMetaImplTest, UnlinkStickyBitFileOwnerCanDelete) {
  // Dir owned by kOther, sticky bit
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);  // sticky + rwx for all
  SetDirOwner(dir_ino, kOther, kGroup);

  // File owned by kOwner (created by kOwner in a writable sticky dir)
  SetContext(kOwner, kOtherGroup);
  impl_->Create(dir_ino, "f", 0644, nullptr);

  // kOwner tries to delete their own file from kOther's sticky dir
  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

TEST_F(MemMetaImplTest, UnlinkStickyBitNonOwnerCannotDelete) {
  // Dir owned by kOther, sticky bit
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);

  // File owned by kOwner
  SetContext(kOwner, kOtherGroup);
  impl_->Create(dir_ino, "f", 0644, nullptr);

  // Now a third user (kOther3) tries to delete kOwner's file
  SetContext(3000, kOtherGroup);
  Status st = impl_->Unlink(dir_ino, "f");
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, UnlinkStickyBitRootCanDelete) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOwner, kOtherGroup);
  impl_->Create(dir_ino, "f", 0644, nullptr);

  // Root can always delete
  SetContext(0, 0);
  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

// ────────────────────────────────────────────────────────────────
// RmDir sticky-bit checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RmDirStickyBitOwnerCanDelete) {
  // Sticky dir owned by kOther, writable for all.
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);

  // kOwner creates a subdirectory inside (so kOwner owns the entry).
  SetContext(kOwner, kOtherGroup);
  InodeID sub_ino = 0;
  ASSERT_TRUE(impl_->MkDir(dir_ino, "sub", 0755, &sub_ino, nullptr).ok());

  // The entry's owner can remove it from someone else's sticky dir.
  EXPECT_TRUE(impl_->RmDir(dir_ino, "sub").ok());
}

TEST_F(MemMetaImplTest, RmDirStickyBitNonOwnerCannotDelete) {
  // Sticky dir owned by kOther, writable for all.
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);

  // kOwner creates a subdirectory inside.
  SetContext(kOwner, kOtherGroup);
  InodeID sub_ino = 0;
  ASSERT_TRUE(impl_->MkDir(dir_ino, "sub", 0755, &sub_ino, nullptr).ok());

  // A third user cannot remove kOwner's subdirectory.
  SetContext(3000, kOtherGroup);
  Status st = impl_->RmDir(dir_ino, "sub");
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// Rename permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RenameRequiresWriteExecOnOldParent) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0777);  // writable for all
  SetContext(kOwner, kOtherGroup);
  impl_->Create(src_ino, "f", 0644, nullptr);

  // Remove write from src
  SetDirMode(src_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNone);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RenameRequiresWriteExecOnNewParent) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0777);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(kOwner, kOtherGroup);
  impl_->Create(src_ino, "f", 0644, nullptr);

  // Remove write from dst
  SetDirMode(dst_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNone);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RenameRootSucceedsRegardlessOfPerms) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0000);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0000);
  SetContext(kOwner, kOtherGroup);
  SetContext(0, 0);
  impl_->Create(src_ino, "f", 0644, nullptr);

  // Root can rename even with no perms on either parent
  SetContext(0, 0);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNone);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, RenameStickyBitNonOwnerCannotMoveOut) {
  // Sticky src dir owned by kOther, writable for all; dst fully open.
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 01777);
  SetDirOwner(src_ino, kOther, kGroup);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0777);

  // kOwner creates the file in the sticky src (so kOwner owns it).
  SetContext(kOwner, kOtherGroup);
  ASSERT_TRUE(impl_->Create(src_ino, "f", 0644, nullptr).ok());

  // A third user cannot move kOwner's file out of kOther's sticky dir.
  SetContext(3000, kOtherGroup);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNone);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RenameStickyBitCannotOverwriteOthersFile) {
  // dst is a sticky dir owned by kOther and already holds kOther's file;
  // src is fully open.
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0777);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 01777);
  SetDirOwner(dst_ino, kOther, kGroup);

  // The victim file in dst is owned by kOther.
  SetContext(kOther, kOtherGroup);
  ASSERT_TRUE(impl_->Create(dst_ino, "f", 0644, nullptr).ok());

  // kOwner's file in src.
  SetContext(kOwner, kOtherGroup);
  ASSERT_TRUE(impl_->Create(src_ino, "f", 0644, nullptr).ok());

  // kOwner may not overwrite kOther's file in kOther's sticky dir.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNone);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// RENAME flags tests
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RenameNoReplaceSucceedsWhenTargetFree) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f_ino = 0;
  CreateFile(src_ino, "f", 0644, &f_ino);

  // RenameFlag::kNoReplace: target "f" under dst does not exist → succeed.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNoReplace);
  EXPECT_TRUE(st.ok()) << st.message();

  // Verify the file moved.
  struct stat attr;
  EXPECT_TRUE(impl_->GetAttr(f_ino, &attr).ok());
}

TEST_F(MemMetaImplTest, RenameNoReplaceFailsWhenTargetExists) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f1_ino = 0, f2_ino = 0;
  CreateFile(src_ino, "f", 0644, &f1_ino);
  CreateFile(dst_ino, "f", 0644, &f2_ino);

  // RenameFlag::kNoReplace: target "f" under dst EXISTS → EEXIST.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNoReplace);
  EXPECT_TRUE(st.IsAlreadyExists()) << st.message();

  // Verify source file was NOT moved (still under src).
  InodeID found = 0;
  EXPECT_TRUE(LookupInode(src_ino, "f", &found).ok());
  EXPECT_EQ(f1_ino, found);
}

TEST_F(MemMetaImplTest, RenameExchangeSucceeds) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f1_ino = 0, f2_ino = 0;
  CreateFile(src_ino, "a", 0644, &f1_ino);
  CreateFile(dst_ino, "b", 0644, &f2_ino);

  // RenameFlag::kExchange: atomically swap "a" and "b".
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RenameFlag::kExchange);
  EXPECT_TRUE(st.ok()) << st.message();

  // Verify: src/a now has inode f2_ino, dst/b now has inode f1_ino.
  InodeID found = 0;
  EXPECT_TRUE(LookupInode(src_ino, "a", &found).ok());
  EXPECT_EQ(f2_ino, found);
  EXPECT_TRUE(LookupInode(dst_ino, "b", &found).ok());
  EXPECT_EQ(f1_ino, found);
}

TEST_F(MemMetaImplTest, RenameExchangeFailsWhenTargetMissing) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  CreateFile(src_ino, "a", 0644);

  // RenameFlag::kExchange: target "b" under dst does NOT exist → ENOENT.
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RenameFlag::kExchange);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

TEST_F(MemMetaImplTest, RenameExchangeFailsTypeMismatch) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  CreateFile(src_ino, "a", 0644);

  // Create a directory under dst with same name.
  MakeDir(dst_ino, "b", 0755);

  // RenameFlag::kExchange: file ↔ dir → EINVAL.
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RenameFlag::kExchange);
  EXPECT_EQ(st.code(), swordfs::utils::Status::kInvalidArgument) << st.message();
}

// ────────────────────────────────────────────────────────────────
// RmDir permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RmDirRequiresWriteExecOnParent) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "parent", 0700);
  SetContext(kOwner, kOtherGroup);
  MakeDir(dir_ino, "sub", 0755);

  // Remove write from parent
  SetDirMode(dir_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->RmDir(dir_ino, "sub");
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RmDirRootAlwaysSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "parent", 0000);
  SetContext(0, 0);
  MakeDir(dir_ino, "sub", 0755);

  SetDirMode(dir_ino, 0000);
  SetContext(0, 0);
  EXPECT_TRUE(impl_->RmDir(dir_ino, "sub").ok());
}

// ────────────────────────────────────────────────────────────────
// Open permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, OpenRequiresReadPermission) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  ASSERT_TRUE(CreateFile(dir_ino, "f", 0644, &f_ino).ok());

  // Remove read from the file owner
  struct stat st {};
  st.st_uid = kOwner;
  st.st_gid = kOtherGroup;
  st.st_mode = S_IFREG | 0200;  // -w-------
  impl_->SetAttr(f_ino, &st, SetAttrField::kUid | SetAttrField::kGid | SetAttrField::kMode, nullptr);

  Status s = impl_->Open(f_ino);
  EXPECT_TRUE(s.IsPermission()) << s.message();
}

TEST_F(MemMetaImplTest, OpenRootSucceedsWithoutReadPerm) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  CreateFile(dir_ino, "f", 0644, &f_ino);

  // Remove all perms
  struct stat st {};
  st.st_mode = S_IFREG | 0000;
  SetContext(0, 0);
  impl_->SetAttr(f_ino, &st, SetAttrField::kMode, nullptr);

  SetContext(0, 0);
  Status s = impl_->Open(f_ino);
  EXPECT_TRUE(s.ok()) << s.message();
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, TruncateNotFound) {
  Status status = impl_->Truncate(999, 0);
  EXPECT_TRUE(status.IsNotFound());
}

TEST_F(MemMetaImplTest, TruncateUpdatesSizeAndClearsSuidSgid) {
  InodeID f_ino = 0;
  ASSERT_TRUE(CreateFile(kRoot, "f", 0644, &f_ino).ok());

  // Give the file SUID/SGID without touching its size.
  struct stat st {};
  st.st_mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(f_ino, &st, SetAttrField::kMode, nullptr).ok());

  ASSERT_TRUE(impl_->Truncate(f_ino, 1024).ok());

  struct stat out {};
  ASSERT_TRUE(impl_->GetAttr(f_ino, &out).ok());
  EXPECT_EQ(out.st_size, 1024);
  EXPECT_EQ(out.st_mode & S_ISUID, 0u);
  EXPECT_EQ(out.st_mode & S_ISGID, 0u);
}

TEST_F(MemMetaImplTest, TruncateSameSizeKeepsSuidSgid) {
  InodeID f_ino = 0;
  ASSERT_TRUE(CreateFile(kRoot, "f", 0644, &f_ino).ok());

  struct stat st {};
  st.st_mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(f_ino, &st, SetAttrField::kMode, nullptr).ok());
  ASSERT_TRUE(impl_->Truncate(f_ino, 0).ok());  // size was already 0

  struct stat out {};
  ASSERT_TRUE(impl_->GetAttr(f_ino, &out).ok());
  EXPECT_EQ(out.st_size, 0);
  EXPECT_NE(out.st_mode & S_ISUID, 0u);
  EXPECT_NE(out.st_mode & S_ISGID, 0u);
}

// ────────────────────────────────────────────────────────────────
// SetAttr size change → Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, SetAttrSizeChangeDelegatesToTruncate) {
  InodeID f_ino = 0;
  ASSERT_TRUE(CreateFile(kRoot, "f", 0644, &f_ino).ok());

  struct stat st {};
  st.st_mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(f_ino, &st, SetAttrField::kMode, nullptr).ok());

  struct stat attr {};
  attr.st_size = 2048;
  swordfs::metadata::SwordFsInode out{};
  ASSERT_TRUE(impl_->SetAttr(f_ino, &attr, SetAttrField::kSize, &out).ok());
  EXPECT_EQ(out.attr.size, 2048);
  EXPECT_EQ(out.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(out.attr.mode & S_ISGID, 0u);
}

TEST_F(MemMetaImplTest, ReclaimInodeMissingInodeIsNoOp) {
  EXPECT_TRUE(impl_->ReclaimInode(999).ok());
}

// Unlink on a hard-linked inode must NOT touch the inode itself — the
// other names (and any chunk objects they share) are still in use. The
// e2e tests FileOpsTest.Hardlink* cover the POSIX contract end-to-end;
// this single-engine test pins the metadata-only invariant.
TEST_F(MemMetaImplTest, UnlinkOnHardlinkedInodeKeepsInodeAlive) {
  InodeID f_ino = 0;
  SetContext(0, 0);
  ASSERT_TRUE(impl_->Create(kRoot, "orig", 0644, &f_ino, nullptr).ok());
  ASSERT_TRUE(impl_->Link(f_ino, kRoot, "link", nullptr).ok());

  // Sanity: both names now point to the same inode, nlink=2.
  struct stat attr {};
  ASSERT_TRUE(impl_->GetAttr(f_ino, &attr).ok());
  ASSERT_EQ(attr.st_nlink, 2);

  ASSERT_TRUE(impl_->Unlink(kRoot, "orig").ok());

  // nlink must drop to 1, not zero, and the inode must still exist.
  ASSERT_TRUE(impl_->GetAttr(f_ino, &attr).ok());
  EXPECT_EQ(attr.st_nlink, 1);

  // Unlinking the surviving name brings nlink to 0 and triggers
  // ReclaimInode, which then drops the inode.
  ASSERT_TRUE(impl_->Unlink(kRoot, "link").ok());
  ASSERT_TRUE(impl_->ReclaimInode(f_ino).ok());
  EXPECT_TRUE(impl_->GetAttr(f_ino, nullptr).IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// Open-unlink behaviour: operations after Unlink (which keeps
// nlink==0 but the inode alive) must continue to work for any fd
// the VFS layer still has open on the inode. The metadata engine
// must NOT refuse these ops just because nlink dropped to 0.
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, OpenAcceptsUnlinkedButLiveInode) {
  // POSIX open-unlink: the directory entry is gone, but the inode
  // stays alive because some fd is still referencing it. Subsequent
  // meta-engine calls on the ino (Open/GetAttr/Access/...) must
  // succeed so the VFS layer can re-open or continue to operate on
  // the fd.
  InodeID f_ino = 0;
  SetContext(0, 0);
  impl_->Create(kRoot, "f", 0644, &f_ino, nullptr);

  // Detach the directory entry (nlink -> 0). The inode survives.
  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());

  // Re-open via the inode number. This is the path /proc, dup-like
  // syscalls, or any "already-have-an-fd" re-bind take. Must succeed.
  EXPECT_TRUE(impl_->Open(f_ino).ok());

  // GetAttr/Access must also succeed so existing fds keep working.
  struct stat attr;
  EXPECT_TRUE(impl_->GetAttr(f_ino, &attr).ok());
  EXPECT_TRUE(impl_->Access(f_ino, R_OK).ok());
  EXPECT_TRUE(impl_->Access(f_ino, W_OK).ok());
}

// ════════════════════════════════════════════════════════════════════
// Concurrency — operation-level atomicity (META-02/META-03)
// ════════════════════════════════════════════════════════════════════
// Every MemMetaImpl method runs as a single MemMetaStore::Transact()
// script, so concurrent observers can never see an intermediate state
// and no SwordFsInode is touched outside a transaction.

// ────────────────────────────────────────────────────────────────
// META-03: rename-overwrite must have no observable gap
// ────────────────────────────────────────────────────────────────
// A rename that overwrites "dst" unlinks the old target and moves the
// source into place as ONE atomic step.  A concurrent Create("dst")
// must therefore ALWAYS fail with AlreadyExists — if it ever succeeds,
// it observed the intermediate state (target gone, source not yet
// moved), which is exactly the data-loss window from META-03.

TEST_F(MemMetaImplTest, ConcurrentRenameOverwriteHasNoObservableGap) {
  constexpr int kRounds = 500;

  SetContext(0, 0);
  ASSERT_TRUE(impl_->Create(kRoot, "dst", 0644, nullptr, nullptr).ok());

  std::atomic<int> create_succeeded{0};
  std::atomic<int> rename_failed{0};
  std::atomic<bool> stop{false};
  std::barrier gate(3);

  // Creator: keeps probing whether "dst" can be created.
  std::thread creator([&]() {
    gate.arrive_and_wait();
    while (!stop.load(std::memory_order_relaxed)) {
      Status status = impl_->Create(kRoot, "dst", 0644, nullptr, nullptr);
      if (status.ok()) {
        create_succeeded.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Renamer: repeatedly overwrites "dst" with a fresh "src".
  std::thread renamer([&]() {
    gate.arrive_and_wait();
    for (int i = 0; i < kRounds; ++i) {
      Status status = impl_->Create(kRoot, "src", 0644, nullptr, nullptr);
      if (!status.ok()) {
        rename_failed.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      status = impl_->Rename(kRoot, "src", kRoot, "dst", RenameFlag::kNone);
      if (!status.ok()) {
        rename_failed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  gate.arrive_and_wait();
  renamer.join();
  stop.store(true, std::memory_order_relaxed);
  creator.join();

  EXPECT_EQ(create_succeeded.load(), 0) << "rename-overwrite was observable mid-flight (META-03)";
  EXPECT_EQ(rename_failed.load(), 0);

  // "dst" must still resolve to a live inode.
  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(kRoot, "dst", &found, nullptr).ok());
}

// ────────────────────────────────────────────────────────────────
// META-02/META-03: concurrent exchanges never lose an inode
// ────────────────────────────────────────────────────────────────
// Two threads keep exchanging a <-> b and b <-> a.  Both names always
// exist, so every exchange must succeed, and afterwards both names must
// resolve to the two original inodes — none may be lost or duplicated.

TEST_F(MemMetaImplTest, ConcurrentExchangeKeepsBothInodes) {
  constexpr int kRounds = 500;

  SetContext(0, 0);
  InodeID a_ino = 0, b_ino = 0;
  ASSERT_TRUE(impl_->Create(kRoot, "a", 0644, &a_ino, nullptr).ok());
  ASSERT_TRUE(impl_->Create(kRoot, "b", 0644, &b_ino, nullptr).ok());

  std::atomic<int> failures{0};
  std::barrier gate(3);

  auto worker = [&](const char *from, const char *to) {
    gate.arrive_and_wait();
    for (int i = 0; i < kRounds; ++i) {
      Status status = impl_->Rename(kRoot, from, kRoot, to, RenameFlag::kExchange);
      if (!status.ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::thread t1(worker, "a", "b");
  std::thread t2(worker, "b", "a");
  gate.arrive_and_wait();
  t1.join();
  t2.join();

  EXPECT_EQ(failures.load(), 0);

  InodeID found_a = 0, found_b = 0;
  ASSERT_TRUE(impl_->Lookup(kRoot, "a", &found_a, nullptr).ok());
  ASSERT_TRUE(impl_->Lookup(kRoot, "b", &found_b, nullptr).ok());
  EXPECT_NE(found_a, found_b);
  EXPECT_TRUE((found_a == a_ino && found_b == b_ino) || (found_a == b_ino && found_b == a_ino));

  // Both inodes must still have readable attributes (no dangling state).
  struct stat attr;
  EXPECT_TRUE(impl_->GetAttr(found_a, &attr).ok());
  EXPECT_TRUE(impl_->GetAttr(found_b, &attr).ok());
}
