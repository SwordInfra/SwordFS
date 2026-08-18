// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaImpl permission checks.

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
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
    impl_ = new MemMetaImpl();
    // Default context is root (uid=0, gid=0).
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override { delete impl_; }

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
    InodeID ino = 0;
    impl_->MkDir(parent_ino, name, mode, &ino, nullptr);
    // Change ownership to kOwner:kGroup
    struct stat st{};
    st.st_uid = kOwner;
    st.st_gid = kGroup;
    st.st_mode = S_IFDIR | mode;
    impl_->SetAttr(ino, &st,
                   FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID | FUSE_SET_ATTR_MODE,
                   nullptr);
    return ino;
  }

  // Change only the mode of an existing directory.
  void SetDirMode(InodeID ino, mode_t mode) {
    SetContext(0, 0);
    struct stat st{};
    st.st_mode = S_IFDIR | mode;
    impl_->SetAttr(ino, &st, FUSE_SET_ATTR_MODE, nullptr);
  }

  // Change ownership of an existing directory.
  void SetDirOwner(InodeID ino, uid_t uid, gid_t gid) {
    SetContext(0, 0);
    struct stat st{};
    st.st_uid = uid;
    st.st_gid = gid;
    impl_->SetAttr(ino, &st, FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID, nullptr);
  }

  MemMetaImpl *impl_;
};

// ────────────────────────────────────────────────────────────────
// Create permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, CreateOwnerWithWriteAndExecSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOwnerWithoutWriteFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0500);  // r-x, no write
  SetContext(kOwner, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOwnerWithoutExecFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0600);  // rw-, no exec
  SetContext(kOwner, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOwnerNoPermsFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);
  SetContext(kOwner, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateRootAlwaysSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);  // no perms at all
  SetContext(0, 0);                                  // root

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateGroupMemberWithWriteExecSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0770);
  // kOther is NOT the owner, but IS in kGroup
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOther, kGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateGroupMemberWithoutWriteFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0750);  // group has r-x only
  // Caller is in kGroup but is NOT the owner (kOwner=1000).
  SetContext(3000, kGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOtherWithWriteExecSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0777);
  // Not owner, not in group
  SetDirOwner(dir_ino, kOwner, kGroup);
  SetContext(kOther, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplTest, CreateOtherWithoutWriteFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0755);  // other has r-x only
  SetDirOwner(dir_ino, kOwner, kGroup);
  SetContext(kOther, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->Create(dir_ino, "f", 0644, &ino, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// MkDir permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, MkDirPermissionDeniedWithoutWrite) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0500);  // no write for owner
  SetContext(kOwner, kOtherGroup);

  InodeID ino = 0;
  Status st = impl_->MkDir(dir_ino, "sub", 0755, &ino, nullptr);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, MkDirRootAlwaysSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0000);
  SetContext(0, 0);

  InodeID ino = 0;
  Status st = impl_->MkDir(dir_ino, "sub", 0755, &ino, nullptr);
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
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

TEST_F(MemMetaImplTest, UnlinkWithoutWriteOnParentFails) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

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
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

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
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

  // kOwner tries to delete their own file from kOther's sticky dir
  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

TEST_F(MemMetaImplTest, UnlinkStickyBitNonOwnerCannotDelete) {
  // Dir owned by kOther, sticky bit
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);

  // File owned by kOwner
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

  // Now a third user (kOther3) tries to delete kOwner's file
  SetContext(3000, kOtherGroup);
  Status st = impl_->Unlink(dir_ino, "f");
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, UnlinkStickyBitRootCanDelete) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

  // Root can always delete
  SetContext(0, 0);
  EXPECT_TRUE(impl_->Unlink(dir_ino, "f").ok());
}

// ────────────────────────────────────────────────────────────────
// Rename permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RenameRequiresWriteExecOnOldParent) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0777);  // writable for all
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  impl_->Create(src_ino, "f", 0644, &f_ino, nullptr);

  // Remove write from src
  SetDirMode(src_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", 0);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RenameRequiresWriteExecOnNewParent) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0777);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  impl_->Create(src_ino, "f", 0644, &f_ino, nullptr);

  // Remove write from dst
  SetDirMode(dst_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", 0);
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RenameRootSucceedsRegardlessOfPerms) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0000);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0000);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  SetContext(0, 0);
  impl_->Create(src_ino, "f", 0644, &f_ino, nullptr);

  // Root can rename even with no perms on either parent
  SetContext(0, 0);
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", 0);
  EXPECT_TRUE(st.ok()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// RENAME flags tests
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RenameNoReplaceSucceedsWhenTargetFree) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f_ino = 0;
  impl_->Create(src_ino, "f", 0644, &f_ino, nullptr);

  // RENAME_NOREPLACE: target "f" under dst does not exist → succeed.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RENAME_NOREPLACE);
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
  impl_->Create(src_ino, "f", 0644, &f1_ino, nullptr);
  impl_->Create(dst_ino, "f", 0644, &f2_ino, nullptr);

  // RENAME_NOREPLACE: target "f" under dst EXISTS → EEXIST.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RENAME_NOREPLACE);
  EXPECT_TRUE(st.IsAlreadyExists()) << st.message();

  // Verify source file was NOT moved (still under src).
  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(src_ino, "f", &found, nullptr).ok());
  EXPECT_EQ(f1_ino, found);
}

TEST_F(MemMetaImplTest, RenameExchangeSucceeds) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f1_ino = 0, f2_ino = 0;
  impl_->Create(src_ino, "a", 0644, &f1_ino, nullptr);
  impl_->Create(dst_ino, "b", 0644, &f2_ino, nullptr);

  // RENAME_EXCHANGE: atomically swap "a" and "b".
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RENAME_EXCHANGE);
  EXPECT_TRUE(st.ok()) << st.message();

  // Verify: src/a now has inode f2_ino, dst/b now has inode f1_ino.
  InodeID found = 0;
  EXPECT_TRUE(impl_->Lookup(src_ino, "a", &found, nullptr).ok());
  EXPECT_EQ(f2_ino, found);
  EXPECT_TRUE(impl_->Lookup(dst_ino, "b", &found, nullptr).ok());
  EXPECT_EQ(f1_ino, found);
}

TEST_F(MemMetaImplTest, RenameExchangeFailsWhenTargetMissing) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f_ino = 0;
  impl_->Create(src_ino, "a", 0644, &f_ino, nullptr);

  // RENAME_EXCHANGE: target "b" under dst does NOT exist → ENOENT.
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RENAME_EXCHANGE);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

TEST_F(MemMetaImplTest, RenameExchangeFailsTypeMismatch) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  InodeID f_ino = 0;
  impl_->Create(src_ino, "a", 0644, &f_ino, nullptr);

  // Create a directory under dst with same name.
  InodeID dir_ino = 0;
  impl_->MkDir(dst_ino, "b", 0755, &dir_ino, nullptr);

  // RENAME_EXCHANGE: file ↔ dir → EINVAL.
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RENAME_EXCHANGE);
  EXPECT_EQ(st.code(), swordfs::utils::Status::kInvalidArgument) << st.message();
}

// ────────────────────────────────────────────────────────────────
// RmDir permission checks
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplTest, RmDirRequiresWriteExecOnParent) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "parent", 0700);
  SetContext(kOwner, kOtherGroup);
  InodeID sub_ino = 0;
  impl_->MkDir(dir_ino, "sub", 0755, &sub_ino, nullptr);

  // Remove write from parent
  SetDirMode(dir_ino, 0500);
  SetContext(kOwner, kOtherGroup);
  Status st = impl_->RmDir(dir_ino, "sub");
  EXPECT_TRUE(st.IsPermission()) << st.message();
}

TEST_F(MemMetaImplTest, RmDirRootAlwaysSucceeds) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "parent", 0000);
  SetContext(0, 0);
  InodeID sub_ino = 0;
  impl_->MkDir(dir_ino, "sub", 0755, &sub_ino, nullptr);

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
  struct stat f_attr;
  impl_->Create(dir_ino, "f", 0644, &f_ino, &f_attr);

  // Remove read from the file owner
  struct stat st{};
  st.st_uid = kOwner;
  st.st_gid = kOtherGroup;
  st.st_mode = S_IFREG | 0200;  // -w-------
  impl_->SetAttr(f_ino, &st,
                 FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID | FUSE_SET_ATTR_MODE,
                 nullptr);

  Status s = impl_->Open(f_ino);
  EXPECT_TRUE(s.IsPermission()) << s.message();
}

TEST_F(MemMetaImplTest, OpenRootSucceedsWithoutReadPerm) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 0700);
  SetContext(kOwner, kOtherGroup);
  InodeID f_ino = 0;
  impl_->Create(dir_ino, "f", 0644, &f_ino, nullptr);

  // Remove all perms
  struct stat st{};
  st.st_mode = S_IFREG | 0000;
  SetContext(0, 0);
  impl_->SetAttr(f_ino, &st, FUSE_SET_ATTR_MODE, nullptr);

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
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &f_ino, nullptr).ok());

  // Give the file SUID/SGID without touching its size.
  struct stat st{};
  st.st_mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(f_ino, &st, FUSE_SET_ATTR_MODE, nullptr).ok());

  ASSERT_TRUE(impl_->Truncate(f_ino, 1024).ok());

  struct stat out{};
  ASSERT_TRUE(impl_->GetAttr(f_ino, &out).ok());
  EXPECT_EQ(out.st_size, 1024);
  EXPECT_EQ(out.st_mode & S_ISUID, 0u);
  EXPECT_EQ(out.st_mode & S_ISGID, 0u);
}

TEST_F(MemMetaImplTest, TruncateSameSizeKeepsSuidSgid) {
  InodeID f_ino = 0;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &f_ino, nullptr).ok());

  struct stat st{};
  st.st_mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(f_ino, &st, FUSE_SET_ATTR_MODE, nullptr).ok());
  ASSERT_TRUE(impl_->Truncate(f_ino, 0).ok());  // size was already 0

  struct stat out{};
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
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &f_ino, nullptr).ok());

  struct stat st{};
  st.st_mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(f_ino, &st, FUSE_SET_ATTR_MODE, nullptr).ok());

  struct stat attr{};
  attr.st_size = 2048;
  struct stat out{};
  ASSERT_TRUE(impl_->SetAttr(f_ino, &attr, FUSE_SET_ATTR_SIZE, &out).ok());
  EXPECT_EQ(out.st_size, 2048);
  EXPECT_EQ(out.st_mode & S_ISUID, 0u);
  EXPECT_EQ(out.st_mode & S_ISGID, 0u);
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
  struct stat attr{};
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
