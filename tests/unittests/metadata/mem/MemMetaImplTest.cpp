// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Tests for MemMetaImpl: permission checks, open-unlink behaviour, and
// operation-level atomicity under concurrency.

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <limits>
#include <thread>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::ReclaimWork;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
static constexpr uid_t kOwner = 1000;
static constexpr uid_t kOther = 2000;
static constexpr gid_t kGroup = 100;
static constexpr gid_t kOtherGroup = 200;
static constexpr uint64_t kChunkSize = 64ULL * 1024 * 1024;

#ifndef NDEBUG
TEST(MemMetaImplDomainTest, RuntimeApiRejectsThreadCaller) {
  MemMetaImpl impl;
  SwordFsInode inode;
  EXPECT_DEATH(
      { (void)impl.GetInode(kRoot, &inode); }, "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}
#endif

class MemMetaImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new MemMetaImpl();
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
    SwordFsAttr attr;
    attr.uid = kOwner;
    attr.gid = kGroup;
    attr.mode = S_IFDIR | mode;
    impl_->SetAttr(ino, attr, SetAttrField::kUid | SetAttrField::kGid | SetAttrField::kMode, nullptr);
    return ino;
  }

  // Change only the mode of an existing directory.
  void SetDirMode(InodeID ino, mode_t mode) {
    SetContext(0, 0);
    SwordFsAttr attr;
    attr.mode = S_IFDIR | mode;
    impl_->SetAttr(ino, attr, SetAttrField::kMode, nullptr);
  }

  // Change ownership of an existing directory.
  void SetDirOwner(InodeID ino, uid_t uid, gid_t gid) {
    SetContext(0, 0);
    SwordFsAttr attr;
    attr.uid = uid;
    attr.gid = gid;
    impl_->SetAttr(ino, attr, SetAttrField::kUid | SetAttrField::kGid, nullptr);
  }

  // ────────────────────────────────────────────────────────────────
  // Reclaim helpers
  // ────────────────────────────────────────────────────────────────

  // The inodes currently published as orphan candidates.
  std::vector<InodeID> OrphanCandidates() {
    std::vector<InodeID> out;
    auto status = impl_->VisitOrphanCandidates([&out](InodeID ino) {
      out.push_back(ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    return out;
  }

  // The inodes with a frozen pending reclaim.
  std::vector<InodeID> PendingReclaims() {
    std::vector<InodeID> out;
    auto status = impl_->VisitPendingReclaims([&out](const ReclaimWork &work) {
      out.push_back(work.ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    return out;
  }

  std::vector<std::string> PendingDeletes() {
    std::vector<std::string> out;
    bool has_more = false;
    auto status = impl_->VisitPendingDeletesBatch(
        1024,
        [&out](const swordfs::metadata::PendingDelete &work) {
          swordfs::chunk::WholeObjectRef ref;
          auto status = swordfs::chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref);
          EXPECT_TRUE(status.ok()) << status.message();
          if (status.ok()) {
            out.push_back(ref.key);
          }
          return Status::OK();
        },
        &has_more);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_FALSE(has_more);
    std::sort(out.begin(), out.end());
    return out;
  }

  MemMetaImpl *impl_;
};

FIBER_TEST_F(MemMetaImplTest, AllocateChunkRevisionIsMonotonicAndStartsAtOne) {
  swordfs::metadata::ChunkRevision first = 0;
  swordfs::metadata::ChunkRevision second = 0;
  swordfs::metadata::ChunkRevision third = 0;
  ASSERT_TRUE(impl_->AllocateChunkRevision(&first).ok());
  ASSERT_TRUE(impl_->AllocateChunkRevision(&second).ok());
  ASSERT_TRUE(impl_->AllocateChunkRevision(&third).ok());
  EXPECT_EQ(first, 1U);
  EXPECT_EQ(second, 2U);
  EXPECT_EQ(third, 3U);
  EXPECT_EQ(impl_->AllocateChunkRevision(nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(MemMetaImplTest, StatFsReportsVirtualInodeCapacity) {
  swordfs::metadata::SwordFsStatFs stat;
  ASSERT_TRUE(impl_->StatFs(&stat).ok());
  const auto limits = impl_->GetLimits();
  EXPECT_GT(stat.files, 0U);
  EXPECT_EQ(stat.files, limits.max_free_inodes);
  EXPECT_EQ(stat.files_free, stat.files);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->StatFs(&stat).ok());
  EXPECT_EQ(stat.files, limits.max_free_inodes);
  EXPECT_EQ(stat.files_free, stat.files);
  EXPECT_EQ(impl_->StatFs(nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(MemMetaImplTest, ReadlinkRejectsNullOutput) {
  SwordFsInode link;
  ASSERT_TRUE(impl_->Symlink(kRoot, "link", "target", &link).ok());

  // IMetaEngine backends share one public contract: callers must receive a
  // status for an invalid output pointer rather than a backend-specific crash.
  EXPECT_EQ(impl_->Readlink(link.ino, nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(MemMetaImplTest, ReadlinkReturnsStoredTargetAndRejectsNonSymlinks) {
  constexpr std::string_view kTarget = "../dir/file";
  SwordFsInode link;
  ASSERT_TRUE(impl_->Symlink(kRoot, "link", kTarget, &link).ok());
  EXPECT_TRUE(link.IsSymlink());
  EXPECT_EQ(link.attr.size, kTarget.size());

  std::string target;
  auto status = impl_->Readlink(link.ino, &target);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(target, kTarget);

  EXPECT_EQ(impl_->Readlink(kRoot, &target).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->Readlink(std::numeric_limits<InodeID>::max(), &target).ToErrno(), ENOENT);
}

FIBER_TEST_F(MemMetaImplTest, NamespaceOperationsRejectOverlongNameComponents) {
  const auto limits = impl_->GetLimits();
  const std::string long_name(limits.max_name_length + 1, 'x');

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "file", 0644, &file).ok());

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, long_name, &found).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Unlink(kRoot, long_name).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->RmDir(kRoot, long_name).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Rename(kRoot, long_name, kRoot, "moved", RenameFlag::kNone).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Rename(kRoot, "file", kRoot, long_name, RenameFlag::kNone).ToErrno() == ENAMETOOLONG);

  EXPECT_TRUE(impl_->Create(kRoot, long_name, 0644, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->MkDir(kRoot, long_name, 0755, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Symlink(kRoot, long_name, "target", nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Link(file.ino, kRoot, long_name, nullptr).ToErrno() == ENAMETOOLONG);
}

FIBER_TEST_F(MemMetaImplTest, MknodPersistsSupportedTypesModeAndDeviceIdentity) {
  struct Case {
    const char *name;
    mode_t mode;
    dev_t rdev;
    dev_t expected_rdev;
  };
  const std::vector<Case> cases = {
      {"regular", S_IFREG | 0601, static_cast<dev_t>(123), 0},
      {"fifo", S_IFIFO | 0620, static_cast<dev_t>(456), 0},
      {"char", S_IFCHR | 0600, static_cast<dev_t>(0x1234), static_cast<dev_t>(0x1234)},
      {"block", S_IFBLK | 0640, static_cast<dev_t>(0x5678), static_cast<dev_t>(0x5678)},
      {"socket", S_IFSOCK | 0770, static_cast<dev_t>(789), 0},
  };

  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx.umask = 0077;

  for (const auto &test_case : cases) {
    SwordFsInode created;
    ASSERT_TRUE(impl_->MkNod(kRoot, test_case.name, test_case.mode, test_case.rdev, &created).ok()) << test_case.name;
    EXPECT_EQ(created.attr.mode, static_cast<uint32_t>(test_case.mode)) << test_case.name;
    EXPECT_EQ(created.attr.rdev, static_cast<uint64_t>(test_case.expected_rdev)) << test_case.name;
    EXPECT_EQ(created.attr.nlink, 1U) << test_case.name;
    EXPECT_EQ(created.attr.size, 0U) << test_case.name;

    SwordFsInode found;
    ASSERT_TRUE(impl_->Lookup(kRoot, test_case.name, &found).ok()) << test_case.name;
    EXPECT_EQ(found.attr.mode, created.attr.mode) << test_case.name;
    EXPECT_EQ(found.attr.rdev, created.attr.rdev) << test_case.name;
  }
}

FIBER_TEST_F(MemMetaImplTest, CreateOwnershipUsesCallerGidWithoutParentSgid) {
  constexpr uid_t kCallerUid = 4101;
  constexpr gid_t kCallerGid = 4102;
  constexpr gid_t kParentGid = 5102;

  InodeID parent_ino = MakeOwnedDir(kRoot, "ordinary-parent", 0755);
  SetDirOwner(parent_ino, kOwner, kParentGid);
  SetContext(kCallerUid, kCallerGid);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(parent_ino, "file", 0644, &file).ok());
  EXPECT_EQ(file.attr.uid, kCallerUid);
  EXPECT_EQ(file.attr.gid, kCallerGid);

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(parent_ino, "dir", 0755, &dir).ok());
  EXPECT_EQ(dir.attr.uid, kCallerUid);
  EXPECT_EQ(dir.attr.gid, kCallerGid);
  EXPECT_EQ(dir.attr.mode & S_ISGID, 0U);

  SwordFsInode fifo;
  ASSERT_TRUE(impl_->MkNod(parent_ino, "fifo", S_IFIFO | 0600, 0, &fifo).ok());
  EXPECT_EQ(fifo.attr.gid, kCallerGid);

  SwordFsInode symlink;
  ASSERT_TRUE(impl_->Symlink(parent_ino, "symlink", "target", &symlink).ok());
  EXPECT_EQ(symlink.attr.gid, kCallerGid);
}

FIBER_TEST_F(MemMetaImplTest, CreateOwnershipInheritsGidAndDirectorySgidFromParent) {
  constexpr uid_t kCallerUid = 4201;
  constexpr gid_t kCallerGid = 4202;
  constexpr gid_t kParentGid = 5202;

  InodeID parent_ino = MakeOwnedDir(kRoot, "sgid-parent", 02775);
  SetDirOwner(parent_ino, kOwner, kParentGid);
  SetDirMode(parent_ino, 02775);
  SetContext(kCallerUid, kCallerGid);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(parent_ino, "file", 0644, &file).ok());
  EXPECT_EQ(file.attr.uid, kCallerUid);
  EXPECT_EQ(file.attr.gid, kParentGid);

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(parent_ino, "dir", 0755, &dir).ok());
  EXPECT_EQ(dir.attr.uid, kCallerUid);
  EXPECT_EQ(dir.attr.gid, kParentGid);
  EXPECT_NE(dir.attr.mode & S_ISGID, 0U);

  SwordFsInode fifo;
  ASSERT_TRUE(impl_->MkNod(parent_ino, "fifo", S_IFIFO | 0600, 0, &fifo).ok());
  EXPECT_EQ(fifo.attr.gid, kParentGid);

  SwordFsInode symlink;
  ASSERT_TRUE(impl_->Symlink(parent_ino, "symlink", "target", &symlink).ok());
  EXPECT_EQ(symlink.attr.gid, kParentGid);
}

FIBER_TEST_F(MemMetaImplTest, MknodReusesNamespaceValidationAndRejectsNonMknodTypes) {
  const std::string long_name(impl_->GetLimits().max_name_length + 1, 'x');
  EXPECT_TRUE(impl_->MkNod(kRoot, long_name, S_IFIFO | 0600, 0, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_EQ(impl_->MkNod(kRoot, "directory", S_IFDIR | 0700, 0, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->MkNod(kRoot, "symlink", S_IFLNK | 0700, 0, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->MkNod(kRoot, "unknown", 0700, 0, nullptr).ToErrno(), EINVAL);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "parent-file", 0644, &file).ok());
  EXPECT_TRUE(impl_->MkNod(file.ino, "child", S_IFIFO | 0600, 0, nullptr).ToErrno() == ENOTDIR);
  EXPECT_TRUE(impl_->MkNod(999999, "missing-parent", S_IFIFO | 0600, 0, nullptr).IsNotFound());

  ASSERT_TRUE(impl_->MkNod(kRoot, "duplicate", S_IFIFO | 0600, 0, nullptr).ok());
  EXPECT_TRUE(impl_->MkNod(kRoot, "duplicate", S_IFIFO | 0600, 0, nullptr).ToErrno() == EEXIST);
}

FIBER_TEST_F(MemMetaImplTest, MknodSpecialNodesUseOrdinaryNamespaceLifecycle) {
  SwordFsInode fifo;
  ASSERT_TRUE(impl_->MkNod(kRoot, "fifo", S_IFIFO | 0600, 0, &fifo).ok());

  swordfs::metadata::DirIteratorPtr iterator;
  ASSERT_TRUE(impl_->OpenDir(kRoot, &iterator).ok());
  bool found_fifo = false;
  for (;;) {
    swordfs::metadata::SwordFsEntry entry;
    uint64_t next_cookie = 0;
    auto status = iterator->Peek(&entry, &next_cookie);
    if (status.IsEndOfDirectory()) {
      break;
    }
    ASSERT_TRUE(status.ok()) << status.message();
    if (entry.name == "fifo") {
      found_fifo = true;
      EXPECT_EQ(entry.type, DT_FIFO);
      EXPECT_EQ(entry.ino, fifo.ino);
    }
    iterator->Advance();
  }
  EXPECT_TRUE(found_fifo);

  ASSERT_TRUE(impl_->Unlink(kRoot, "fifo").ok());
  EXPECT_EQ(OrphanCandidates(), std::vector<InodeID>{fifo.ino});

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(fifo.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kChunkSize, &refs).ok());
  EXPECT_TRUE(refs.empty());
  SwordFsInode reclaimed;
  EXPECT_TRUE(impl_->GetInode(fifo.ino, &reclaimed).IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// Kernel-DAC boundary
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaImplTest, MetadataDoesNotDuplicateKernelDac) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "d", 01000);
  SetContext(kOther, kOtherGroup);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(dir_ino, "f", 0000, &file).ok());
  uint64_t open_size = 1;
  ASSERT_TRUE(impl_->Open(file.ino, &open_size).ok());
  EXPECT_EQ(open_size, 0U);
  ASSERT_TRUE(impl_->MkDir(dir_ino, "sub", 0000, nullptr).ok());
  ASSERT_TRUE(impl_->Symlink(dir_ino, "sym", "target", nullptr).ok());
  ASSERT_TRUE(impl_->Link(file.ino, dir_ino, "hard", nullptr).ok());
  ASSERT_TRUE(impl_->Rename(dir_ino, "f", dir_ino, "renamed", RenameFlag::kNone).ok());
  ASSERT_TRUE(impl_->Unlink(dir_ino, "renamed").ok());
  ASSERT_TRUE(impl_->RmDir(dir_ino, "sub").ok());
}

FIBER_TEST_F(MemMetaImplTest, StickyDirectoryOwnershipSafetyRemainsInMetadata) {
  InodeID sticky_unlink_ino = MakeOwnedDir(kRoot, "sticky-unlink", 01777);
  SwordFsInode unlink_target;
  ASSERT_TRUE(impl_->Create(sticky_unlink_ino, "file", 0644, &unlink_target).ok());

  InodeID sticky_rmdir_ino = MakeOwnedDir(kRoot, "sticky-rmdir", 01777);
  ASSERT_TRUE(impl_->MkDir(sticky_rmdir_ino, "subdir", 0755, nullptr).ok());

  InodeID sticky_source_ino = MakeOwnedDir(kRoot, "sticky-source", 01777);
  InodeID plain_dest_ino = MakeOwnedDir(kRoot, "plain-dest", 0777);
  ASSERT_TRUE(impl_->Create(sticky_source_ino, "source", 0644, nullptr).ok());

  InodeID plain_source_ino = MakeOwnedDir(kRoot, "plain-source", 0777);
  InodeID sticky_dest_ino = MakeOwnedDir(kRoot, "sticky-dest", 01777);
  ASSERT_TRUE(impl_->Create(plain_source_ino, "source", 0644, nullptr).ok());
  ASSERT_TRUE(impl_->Create(sticky_dest_ino, "target", 0644, nullptr).ok());

  SetContext(kOther, kOtherGroup);
  EXPECT_TRUE(impl_->Unlink(sticky_unlink_ino, "file").ToErrno() == EACCES);
  EXPECT_TRUE(impl_->RmDir(sticky_rmdir_ino, "subdir").ToErrno() == EACCES);
  EXPECT_TRUE(impl_->Rename(sticky_source_ino, "source", plain_dest_ino, "moved", RenameFlag::kNone).ToErrno() ==
              EACCES);
  EXPECT_TRUE(impl_->Rename(plain_source_ino, "source", sticky_dest_ino, "target", RenameFlag::kNone).ToErrno() ==
              EACCES);
}

FIBER_TEST_F(MemMetaImplTest, StickyDirectoryOwnerCanUnlinkEntry) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "sticky-owner", 01700);
  SetContext(kOwner, kOtherGroup);
  ASSERT_TRUE(impl_->Create(dir_ino, "file", 0644, nullptr).ok());

  EXPECT_TRUE(impl_->Unlink(dir_ino, "file").ok());
}

FIBER_TEST_F(MemMetaImplTest, StickyEntryOwnerCanUnlinkFromAnotherOwnersDirectory) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "sticky-file-owner", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOwner, kOtherGroup);
  ASSERT_TRUE(impl_->Create(dir_ino, "file", 0644, nullptr).ok());

  EXPECT_TRUE(impl_->Unlink(dir_ino, "file").ok());
}

FIBER_TEST_F(MemMetaImplTest, RootCanUnlinkFromStickyDirectory) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "sticky-root", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOwner, kOtherGroup);
  ASSERT_TRUE(impl_->Create(dir_ino, "file", 0644, nullptr).ok());

  SetContext(0, 0);
  EXPECT_TRUE(impl_->Unlink(dir_ino, "file").ok());
}

FIBER_TEST_F(MemMetaImplTest, StickyEntryOwnerCanRemoveOwnDirectory) {
  InodeID dir_ino = MakeOwnedDir(kRoot, "sticky-rmdir-owner", 01777);
  SetDirOwner(dir_ino, kOther, kGroup);
  SetContext(kOwner, kOtherGroup);
  ASSERT_TRUE(impl_->MkDir(dir_ino, "subdir", 0755, nullptr).ok());

  EXPECT_TRUE(impl_->RmDir(dir_ino, "subdir").ok());
}

// ────────────────────────────────────────────────────────────────
// RENAME flags tests
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaImplTest, RenameNoReplaceSucceedsWhenTargetFree) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(src_ino, "f", 0644, &file).ok());

  // RenameFlag::kNoReplace: target "f" under dst does not exist → succeed.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNoReplace);
  EXPECT_TRUE(st.ok()) << st.message();

  // Verify the file moved.
  SwordFsInode inode;
  EXPECT_TRUE(impl_->GetInode(file.ino, &inode).ok());
}

FIBER_TEST_F(MemMetaImplTest, RenameNoReplaceFailsWhenTargetExists) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  SwordFsInode src_file;
  SwordFsInode dst_file;
  ASSERT_TRUE(impl_->Create(src_ino, "f", 0644, &src_file).ok());
  ASSERT_TRUE(impl_->Create(dst_ino, "f", 0644, &dst_file).ok());

  // RenameFlag::kNoReplace: target "f" under dst EXISTS → EEXIST.
  Status st = impl_->Rename(src_ino, "f", dst_ino, "f", RenameFlag::kNoReplace);
  EXPECT_TRUE(st.ToErrno() == EEXIST) << st.message();

  // Verify source file was NOT moved (still under src).
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(src_ino, "f", &found).ok());
  EXPECT_EQ(src_file.ino, found.ino);
}

FIBER_TEST_F(MemMetaImplTest, RenameExchangeSucceeds) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  SwordFsInode src_file;
  SwordFsInode dst_file;
  ASSERT_TRUE(impl_->Create(src_ino, "a", 0644, &src_file).ok());
  ASSERT_TRUE(impl_->Create(dst_ino, "b", 0644, &dst_file).ok());

  // RenameFlag::kExchange: atomically swap "a" and "b".
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RenameFlag::kExchange);
  EXPECT_TRUE(st.ok()) << st.message();

  // Verify: src/a now has inode f2_ino, dst/b now has inode f1_ino.
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(src_ino, "a", &found).ok());
  EXPECT_EQ(dst_file.ino, found.ino);
  EXPECT_TRUE(impl_->Lookup(dst_ino, "b", &found).ok());
  EXPECT_EQ(src_file.ino, found.ino);
}

FIBER_TEST_F(MemMetaImplTest, RenameExchangeFailsWhenTargetMissing) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  ASSERT_TRUE(impl_->Create(src_ino, "a", 0644, nullptr).ok());

  // RenameFlag::kExchange: target "b" under dst does NOT exist → ENOENT.
  Status st = impl_->Rename(src_ino, "a", dst_ino, "b", RenameFlag::kExchange);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

FIBER_TEST_F(MemMetaImplTest, RenameExchangeFileAndDirectoryAcrossParentsUpdatesTopology) {
  InodeID src_ino = MakeOwnedDir(kRoot, "src", 0700);
  InodeID dst_ino = MakeOwnedDir(kRoot, "dst", 0700);
  SetContext(0, 0);
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(src_ino, "a", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(dst_ino, "b", 0755, &dir).ok());

  SwordFsInode src_before;
  SwordFsInode dst_before;
  ASSERT_TRUE(impl_->GetInode(src_ino, &src_before).ok());
  ASSERT_TRUE(impl_->GetInode(dst_ino, &dst_before).ok());

  Status status = impl_->Rename(src_ino, "a", dst_ino, "b", RenameFlag::kExchange);
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(src_ino, "a", &found).ok());
  EXPECT_EQ(found.ino, dir.ino);
  EXPECT_EQ(found.parent_ino, src_ino);
  ASSERT_TRUE(impl_->Lookup(dst_ino, "b", &found).ok());
  EXPECT_EQ(found.ino, file.ino);

  SwordFsInode src_after;
  SwordFsInode dst_after;
  ASSERT_TRUE(impl_->GetInode(src_ino, &src_after).ok());
  ASSERT_TRUE(impl_->GetInode(dst_ino, &dst_after).ok());
  EXPECT_EQ(src_after.attr.nlink, src_before.attr.nlink + 1);
  EXPECT_EQ(dst_after.attr.nlink, dst_before.attr.nlink - 1);
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaImplTest, TruncateNotFound) {
  Status status = impl_->Truncate(999, 0);
  EXPECT_TRUE(status.IsNotFound());
}

FIBER_TEST_F(MemMetaImplTest, TruncateUpdatesSizeAndClearsSuidSgid) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());

  // Give the file SUID/SGID without touching its size.
  SwordFsAttr attr;
  attr.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, attr, SetAttrField::kMode, nullptr).ok());

  ASSERT_TRUE(impl_->Truncate(file.ino, 1024).ok());

  SwordFsInode inode;
  ASSERT_TRUE(impl_->GetInode(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 1024U);
  EXPECT_EQ(inode.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(inode.attr.mode & S_ISGID, 0u);
}

FIBER_TEST_F(MemMetaImplTest, TruncateSameSizeKeepsSuidSgid) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());

  SwordFsAttr attr;
  attr.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, attr, SetAttrField::kMode, nullptr).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 0).ok());  // size was already 0

  SwordFsInode inode;
  ASSERT_TRUE(impl_->GetInode(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 0U);
  EXPECT_NE(inode.attr.mode & S_ISUID, 0u);
  EXPECT_NE(inode.attr.mode & S_ISGID, 0u);
}

// ────────────────────────────────────────────────────────────────
// SetAttr size change → Truncate
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaImplTest, SetAttrSizeChangeDelegatesToTruncate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());

  SwordFsAttr mode;
  mode.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, mode, SetAttrField::kMode, nullptr).ok());

  SwordFsAttr attr;
  attr.size = 2048;
  SwordFsInode out;
  ASSERT_TRUE(impl_->SetAttr(file.ino, attr, SetAttrField::kSize, &out).ok());
  EXPECT_EQ(out.attr.size, 2048);
  EXPECT_EQ(out.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(out.attr.mode & S_ISGID, 0u);
}

FIBER_TEST_F(MemMetaImplTest, CommitChunkInitialPublishIsIdempotentAndGrowsSizeMonotonically) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "publish", 0644, &file).ok());

  SwordFsChunk first{};
  first.index = 0;
  first.revision = 1;
  first.size = 128;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  SwordFsInode inode;
  ASSERT_TRUE(impl_->GetInode(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 128U);

  SwordFsChunk later{};
  later.index = 2;
  later.revision = 2;
  later.size = 64;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, later).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 2 * kChunkSize + 64);

  auto conflicting = first;
  conflicting.revision = 3;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, conflicting).ToErrno() == EEXIST);

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored.revision, first.revision);
}

FIBER_TEST_F(MemMetaImplTest, LoadChunkViewReturnsPublishedHeadAndWholeObjectSnapshot) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "chunk-view", 0644, &file).ok());
  const SwordFsChunk head{.index = 0, .revision = 1, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, head).ok());

  swordfs::metadata::ChunkView view;
  ASSERT_TRUE(impl_->LoadChunkView(file.ino, 0, &view).ok());
  EXPECT_EQ(view.head, head);
  EXPECT_TRUE(view.private_snapshot.empty());
  EXPECT_TRUE(impl_->LoadChunkView(file.ino, 1, &view).IsNotFound());
  EXPECT_EQ(impl_->LoadChunkView(file.ino, 0, nullptr).ToErrno(), EINVAL);

  const SwordFsChunk replacement{.index = 0, .revision = 2, .size = 128};
  const swordfs::metadata::ChunkPublishIntent unexpected{.payload = "not-whole-object"};
  EXPECT_EQ(impl_->CommitChunk(file.ino, head, replacement, unexpected).ToErrno(), EINVAL);
  ASSERT_TRUE(impl_->LoadChunkView(file.ino, 0, &view).ok());
  EXPECT_EQ(view.head, head);
}

FIBER_TEST_F(MemMetaImplTest, CommitChunkRewriteUsesCompareAndSwapAndIsIdempotent) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "replace", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .revision = 1, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  SwordFsAttr mode;
  mode.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, mode, SetAttrField::kMode, nullptr).ok());

  auto replacement = first;
  replacement.revision = 2;
  replacement.size = 64;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());
  EXPECT_EQ(PendingDeletes(),
            std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision)});
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, replacement);

  SwordFsInode inode;
  ASSERT_TRUE(impl_->GetInode(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 128U);
  EXPECT_EQ(inode.attr.mode & (S_ISUID | S_ISGID), 0U);

  auto stale_replacement = replacement;
  stale_replacement.revision = 3;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, first, stale_replacement).ToErrno() == EEXIST);
  EXPECT_EQ(PendingDeletes(),
            (std::vector<std::string>{
                swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision),
                swordfs::chunk::FormatChunkObjectKey(file.ino, stale_replacement.index, stale_replacement.revision)}));

  auto grown = replacement;
  grown.revision = 4;
  grown.size = 256;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, replacement, grown).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 256U);
}

FIBER_TEST_F(MemMetaImplTest, CommitChunkRejectsInvalidTargetsAndDescriptors) {
  SwordFsChunk expected{.index = 0, .revision = 1, .size = 64};
  auto replacement = expected;
  replacement.revision = 2;

  auto invalid_revision = expected;
  invalid_revision.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_EQ(impl_->CommitChunk(999999, invalid_revision, replacement).ToErrno(), EINVAL);

  auto invalid_replacement = replacement;
  invalid_replacement.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_EQ(impl_->CommitChunk(999999, expected, invalid_replacement).ToErrno(), EINVAL);

  auto oversized_replacement = replacement;
  oversized_replacement.size = kChunkSize + 1;
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, oversized_replacement).ToErrno(), EINVAL);

  EXPECT_TRUE(impl_->CommitChunk(999999, expected, replacement).IsNotFound());

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRoot, "replace-dir", 0755, &dir).ok());
  EXPECT_EQ(impl_->CommitChunk(dir.ino, expected, replacement).ToErrno(), EINVAL);

  SwordFsInode empty_file;
  ASSERT_TRUE(impl_->Create(kRoot, "replace-empty", 0644, &empty_file).ok());
  EXPECT_TRUE(impl_->CommitChunk(empty_file.ino, expected, replacement).IsNotFound());

  auto mismatched = replacement;
  mismatched.index = 1;
  EXPECT_EQ(impl_->CommitChunk(empty_file.ino, expected, mismatched).ToErrno(), EINVAL);

  auto stale_revision = replacement;
  stale_revision.revision = expected.revision;
  EXPECT_EQ(impl_->CommitChunk(empty_file.ino, expected, stale_revision).ToErrno(), EINVAL);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "replace-missing-index", 0644, &file).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, expected).ok());
  SwordFsChunk missing{.index = 1, .revision = 3, .size = 64};
  auto missing_replacement = missing;
  missing_replacement.revision = 4;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, missing, missing_replacement).IsNotFound());
}

FIBER_TEST_F(MemMetaImplTest, ChunkMutationsRejectInvalidRevision) {
  SwordFsChunk invalid{.index = 0, .revision = swordfs::metadata::kInvalidChunkRevision, .size = 64};
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, invalid).ToErrno(), EINVAL);
}

FIBER_TEST_F(MemMetaImplTest, PrepareReclaimMissingInodeIsNoOp) {
  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(999, &work).ok());
  EXPECT_FALSE(work.has_value());
  // Completing a reclaim nobody prepared is a no-op.
  EXPECT_TRUE(impl_->CompleteReclaim(999).ok());
}

// Unlink on a hard-linked inode must NOT touch the inode itself — the
// other names (and any chunk objects they share) are still in use. The
// e2e tests FileOpsTest.Hardlink* cover the POSIX contract end-to-end;
// this single-engine test pins the metadata-only invariant.
FIBER_TEST_F(MemMetaImplTest, UnlinkOnHardlinkedInodeKeepsInodeAlive) {
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "orig", 0644, &file).ok());
  ASSERT_TRUE(impl_->Link(file.ino, kRoot, "link", nullptr).ok());

  // Sanity: both names now point to the same inode, nlink=2.
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  ASSERT_EQ(file.attr.nlink, 2U);

  ASSERT_TRUE(impl_->Unlink(kRoot, "orig").ok());

  // nlink must drop to 1, not zero, and the inode must still exist.
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.nlink, 1U);
  EXPECT_TRUE(OrphanCandidates().empty());

  // Unlinking the surviving name brings nlink to 0, which publishes the
  // inode as an orphan candidate; preparing and completing the reclaim then
  // drops the inode.
  ASSERT_TRUE(impl_->Unlink(kRoot, "link").ok());
  EXPECT_EQ(OrphanCandidates(), std::vector<InodeID>{file.ino});
  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  SwordFsInode missing;
  EXPECT_TRUE(impl_->GetInode(file.ino, &missing).IsNotFound());
}

// ────────────────────────────────────────────────────────────────
// Open-unlink behaviour: operations after Unlink (which keeps
// nlink==0 but the inode alive) must continue to work for any fd
// the VFS layer still has open on the inode. The metadata engine
// must NOT refuse these ops just because nlink dropped to 0.
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(MemMetaImplTest, OpenAcceptsUnlinkedButLiveInode) {
  // POSIX open-unlink: the directory entry is gone, but the inode
  // stays alive because some fd is still referencing it. Subsequent
  // meta-engine calls on the ino (Open/GetInode/...) must
  // succeed so the VFS layer can re-open or continue to operate on
  // the fd.
  SetContext(0, 0);
  SwordFsInode file;
  impl_->Create(kRoot, "f", 0644, &file);

  // Detach the directory entry (nlink -> 0). The inode survives.
  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());

  // Re-open via the inode number. This is the path /proc, dup-like
  // syscalls, or any "already-have-an-fd" re-bind take. Must succeed.
  EXPECT_TRUE(impl_->Open(file.ino).ok());

  // GetInode must also succeed so existing fds keep working.
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).ok());
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

FIBER_TEST_F(MemMetaImplTest, ConcurrentRenameOverwriteHasNoObservableGap) {
  constexpr int kRounds = 500;

  SetContext(0, 0);
  ASSERT_TRUE(impl_->Create(kRoot, "dst", 0644, nullptr).ok());

  std::atomic<int> create_succeeded{0};
  std::atomic<int> rename_failed{0};
  std::atomic<bool> stop{false};
  std::barrier gate(3);

  // Creator: keeps probing whether "dst" can be created.
  auto creator = swordfs::test::StartFiberTestThread([&]() {
    gate.arrive_and_wait();
    while (!stop.load(std::memory_order_relaxed)) {
      Status status = impl_->Create(kRoot, "dst", 0644, nullptr);
      if (status.ok()) {
        create_succeeded.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Renamer: repeatedly overwrites "dst" with a fresh "src".
  auto renamer = swordfs::test::StartFiberTestThread([&]() {
    gate.arrive_and_wait();
    for (int i = 0; i < kRounds; ++i) {
      Status status = impl_->Create(kRoot, "src", 0644, nullptr);
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
  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRoot, "dst", &found).ok());
}

// ────────────────────────────────────────────────────────────────
// META-02/META-03: concurrent exchanges never lose an inode
// ────────────────────────────────────────────────────────────────
// Two threads keep exchanging a <-> b and b <-> a.  Both names always
// exist, so every exchange must succeed, and afterwards both names must
// resolve to the two original inodes — none may be lost or duplicated.

FIBER_TEST_F(MemMetaImplTest, ConcurrentExchangeKeepsBothInodes) {
  constexpr int kRounds = 500;

  SetContext(0, 0);
  SwordFsInode a;
  SwordFsInode b;
  ASSERT_TRUE(impl_->Create(kRoot, "a", 0644, &a).ok());
  ASSERT_TRUE(impl_->Create(kRoot, "b", 0644, &b).ok());

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

  auto t1 = swordfs::test::StartFiberTestThread(worker, "a", "b");
  auto t2 = swordfs::test::StartFiberTestThread(worker, "b", "a");
  gate.arrive_and_wait();
  t1.join();
  t2.join();

  EXPECT_EQ(failures.load(), 0);

  SwordFsInode found_a;
  SwordFsInode found_b;
  ASSERT_TRUE(impl_->Lookup(kRoot, "a", &found_a).ok());
  ASSERT_TRUE(impl_->Lookup(kRoot, "b", &found_b).ok());
  EXPECT_NE(found_a.ino, found_b.ino);
  EXPECT_TRUE((found_a.ino == a.ino && found_b.ino == b.ino) || (found_a.ino == b.ino && found_b.ino == a.ino));

  // Both inodes must still have readable attributes (no dangling state).
  EXPECT_TRUE(impl_->GetInode(found_a.ino, &found_a).ok());
  EXPECT_TRUE(impl_->GetInode(found_b.ino, &found_b).ok());
}

// ════════════════════════════════════════════════════════════════════
// Reclaim — orphan candidates and the point of no return
// ════════════════════════════════════════════════════════════════════
//
// The memory backend mirrors the persistent backends' reclaim contract for
// the process lifetime: the mutation that drops an inode's nlink to zero
// publishes it as an orphan candidate, reclaim preparation rechecks nlink
// and freezes the authoritative object identities, and completion removes
// the frozen record once the caller deleted them.

FIBER_TEST_F(MemMetaImplTest, UnlinkPublishesOrphanCandidateForLastLink) {
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  const InodeID f_ino = file.ino;
  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, chunk).ok());

  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());

  // The candidate is published in the same transaction as the nlink
  // decrement, and the inode with its chunk metadata is still alive: the
  // candidate is a promise to reclaim, not a claim on the data.
  EXPECT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});
  SwordFsInode inode;
  ASSERT_TRUE(impl_->GetInode(f_ino, &inode).ok());
  EXPECT_EQ(inode.attr.nlink, 0U);
  SwordFsChunk found;
  ASSERT_TRUE(impl_->FindChunk(f_ino, 0, &found).ok());
  EXPECT_EQ(found, chunk);
  EXPECT_TRUE(PendingReclaims().empty());

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(f_ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  ASSERT_TRUE(impl_->CompleteReclaim(f_ino).ok());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_TRUE(impl_->GetInode(f_ino, &inode).IsNotFound());
}

FIBER_TEST_F(MemMetaImplTest, HardlinkUnlinkKeepsInodeOffTheOrphanList) {
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "a", 0644, &file).ok());
  const InodeID f_ino = file.ino;
  ASSERT_TRUE(impl_->Link(f_ino, kRoot, "b", nullptr).ok());

  // One name gone, one left: the inode is not an orphan candidate.
  ASSERT_TRUE(impl_->Unlink(kRoot, "a").ok());
  EXPECT_TRUE(OrphanCandidates().empty());

  // Last name gone: now it is.
  ASSERT_TRUE(impl_->Unlink(kRoot, "b").ok());
  EXPECT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});
}

FIBER_TEST_F(MemMetaImplTest, RenameOverwritePublishesOrphanCandidate) {
  SetContext(0, 0);
  SwordFsInode dst;
  SwordFsInode src;
  ASSERT_TRUE(impl_->Create(kRoot, "dst", 0644, &dst).ok());
  ASSERT_TRUE(impl_->Create(kRoot, "src", 0644, &src).ok());
  const InodeID dst_ino = dst.ino;
  const InodeID src_ino = src.ino;
  ASSERT_TRUE(impl_->CommitChunk(dst_ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 32}).ok());

  // Renaming src onto dst overwrites dst exactly like unlink(2) would.
  ASSERT_TRUE(impl_->Rename(kRoot, "src", kRoot, "dst", RenameFlag::kNone).ok());

  EXPECT_EQ(OrphanCandidates(), std::vector<InodeID>{dst_ino});
  // The overwritten inode keeps its chunk metadata until the reclaim is
  // prepared, so a racing Link (or an open fd) cannot observe lost data.
  SwordFsChunk found;
  EXPECT_TRUE(impl_->FindChunk(dst_ino, 0, &found).ok());
  SwordFsInode resolved;
  ASSERT_TRUE(impl_->Lookup(kRoot, "dst", &resolved).ok());
  EXPECT_EQ(resolved.ino, src_ino);
}

FIBER_TEST_F(MemMetaImplTest, LinkCancelsOrphanCandidate) {
  // A hard link that lands before the reclaim's transaction revives the
  // inode: preparation must not freeze anything, and the marker must be gone.
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  const InodeID f_ino = file.ino;
  ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 16}).ok());
  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());
  ASSERT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});

  ASSERT_TRUE(impl_->Link(f_ino, kRoot, "revived", nullptr).ok());

  EXPECT_TRUE(OrphanCandidates().empty());
  SwordFsInode inode;
  ASSERT_TRUE(impl_->GetInode(f_ino, &inode).ok());
  EXPECT_EQ(inode.attr.nlink, 1U);

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(f_ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  // Untouched: the revived name still owns the inode and its data.
  SwordFsChunk found;
  ASSERT_TRUE(impl_->FindChunk(f_ino, 0, &found).ok());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(MemMetaImplTest, PrepareReclaimFreezesAuthoritativeRevisionAndFencesLink) {
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  const InodeID f_ino = file.ino;

  // Publish two chunks out of index order: the frozen work must be complete
  // and ordered, and every key must be the revisioned identity of the
  // authoritative descriptor.
  ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, SwordFsChunk{.index = 1, .revision = 2, .size = 128}).ok());
  ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 4096}).ok());
  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(f_ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  EXPECT_EQ(work->ino, f_ino);
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 2U);
  EXPECT_EQ(refs[0].descriptor.index, 0U);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(f_ino, 0, 1));
  EXPECT_EQ(refs[1].descriptor.index, 1U);
  EXPECT_EQ(refs[1].key, swordfs::chunk::FormatChunkObjectKey(f_ino, 1, 2));

  // The live inode and its chunk metadata are gone, so no name and no
  // inode-by-number reference can reach the data again...
  SwordFsInode missing;
  EXPECT_TRUE(impl_->GetInode(f_ino, &missing).IsNotFound());
  SwordFsChunk found;
  EXPECT_TRUE(impl_->FindChunk(f_ino, 0, &found).IsNotFound());
  // ... and a Link can no longer revive an inode whose objects are frozen.
  EXPECT_TRUE(impl_->Link(f_ino, kRoot, "revived", nullptr).IsNotFound());

  // The frozen record is the authority until completion; replaying
  // preparation returns the same work.
  EXPECT_EQ(PendingReclaims(), std::vector<InodeID>{f_ino});
  EXPECT_TRUE(OrphanCandidates().empty());
  std::optional<ReclaimWork> replay;
  ASSERT_TRUE(impl_->PrepareReclaim(f_ino, &replay).ok());
  EXPECT_EQ(replay, work);

  ASSERT_TRUE(impl_->CompleteReclaim(f_ino).ok());
  EXPECT_TRUE(PendingReclaims().empty());
  // Completion is idempotent.
  EXPECT_TRUE(impl_->CompleteReclaim(f_ino).ok());
}

FIBER_TEST_F(MemMetaImplTest, PrepareReclaimUsesCurrentRevisionAfterRewrite) {
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  const InodeID f_ino = file.ino;
  ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());

  SwordFsChunk first;
  ASSERT_TRUE(impl_->FindChunk(f_ino, 0, &first).ok());
  SwordFsChunk replacement = first;
  replacement.revision = first.revision + 1;
  replacement.size = 128;
  ASSERT_TRUE(impl_->CommitChunk(f_ino, first, replacement).ok());

  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());
  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(f_ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  // The frozen identity is the current revisioned key, never the retired one.
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(f_ino, 0, replacement.revision));
  EXPECT_NE(refs[0].key, swordfs::chunk::FormatChunkObjectKey(f_ino, 0, first.revision));
}

FIBER_TEST_F(MemMetaImplTest, PrepareReclaimRejectsLinkedInode) {
  SetContext(0, 0);
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  const InodeID f_ino = file.ino;
  ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 8}).ok());

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(f_ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  // Nothing was removed: the file still resolves and its chunk is intact.
  SwordFsChunk found;
  ASSERT_TRUE(impl_->FindChunk(f_ino, 0, &found).ok());
  EXPECT_TRUE(PendingReclaims().empty());
  EXPECT_TRUE(OrphanCandidates().empty());
}

// ────────────────────────────────────────────────────────────────
// #141: reclaim vs Link must be atomic
// ────────────────────────────────────────────────────────────────
// A hard link can arrive for an inode whose last name was just removed
// (linkat(AT_EMPTY_PATH)). Whatever the interleaving, exactly one of two
// states may survive: the link revived the inode — and its chunk metadata
// must still be there for the revived name — or the reclaim passed its point
// of no return, in which case the link must fail. A link that succeeded over
// reclaimed objects would be silent data loss.

FIBER_TEST_F(MemMetaImplTest, ConcurrentReclaimAndLinkAreAtomic) {
  constexpr int kRounds = 200;
  SetContext(0, 0);

  std::atomic<int> revived{0};
  std::atomic<int> reclaimed{0};
  std::atomic<int> failures{0};

  for (int round = 0; round < kRounds; ++round) {
    SwordFsInode file;
    ASSERT_TRUE(impl_->Create(kRoot, "race", 0644, &file).ok());
    const InodeID f_ino = file.ino;
    ASSERT_TRUE(impl_->CommitChunk(f_ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());
    ASSERT_TRUE(impl_->Unlink(kRoot, "race").ok());

    std::barrier gate(3);
    std::atomic<bool> link_won{false};
    std::atomic<bool> reclaim_won{false};

    auto linker = swordfs::test::StartFiberTestThread([&] {
      gate.arrive_and_wait();
      SwordFsInode inode;
      if (impl_->Link(f_ino, kRoot, "revived", &inode).ok()) {
        link_won.store(true);
      }
    });
    auto reclaimer = swordfs::test::StartFiberTestThread([&] {
      gate.arrive_and_wait();
      std::optional<ReclaimWork> work;
      const auto status = impl_->PrepareReclaim(f_ino, &work);
      if (status.ok() && work.has_value()) {
        reclaim_won.store(true);
        if (!impl_->CompleteReclaim(f_ino).ok()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      } else if (!status.ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    });

    gate.arrive_and_wait();
    linker.join();
    reclaimer.join();

    const bool linked = link_won.load();
    const bool prepared = reclaim_won.load();
    ASSERT_NE(linked, prepared) << "round " << round << ": exactly one of Link and reclaim may win";

    if (linked) {
      // The revived inode must still own its data.
      SwordFsInode inode;
      ASSERT_TRUE(impl_->GetInode(f_ino, &inode).ok());
      EXPECT_EQ(inode.attr.nlink, 1U);
      SwordFsChunk found;
      EXPECT_TRUE(impl_->FindChunk(f_ino, 0, &found).ok()) << "a revived inode must never lose its chunk metadata";
      revived.fetch_add(1, std::memory_order_relaxed);
      // Reset for the next round.
      EXPECT_TRUE(impl_->Unlink(kRoot, "revived").ok());
    } else {
      SwordFsInode missing;
      EXPECT_TRUE(impl_->GetInode(f_ino, &missing).IsNotFound());
      reclaimed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  EXPECT_EQ(revived.load() + reclaimed.load(), kRounds);
  EXPECT_EQ(failures.load(), 0);
}

// ────────────────────────────────────────────────────────────────
// #143: visitor and output validation
// ────────────────────────────────────────────────────────────────
// Reconciliation drives reclamation through the visitors, so a caller bug
// (null visitor, null output) must be refused explicitly rather than read as
// "nothing to reclaim" — an empty scan is a legitimate, different answer.

FIBER_TEST_F(MemMetaImplTest, VisitorAndOutputArgumentsAreValidated) {
  SetContext(0, 0);

  EXPECT_EQ(impl_->VisitOrphanCandidates(swordfs::metadata::InodeVisitorFn{}).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->VisitPendingReclaims(swordfs::metadata::ReclaimVisitorFn{}).ToErrno(), EINVAL);
  bool has_more = false;
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, swordfs::metadata::PendingDeleteVisitorFn{}, &has_more).ToErrno(),
            EINVAL);

  // A null frozen-work output is refused before anything is mutated.
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "f", 0644, &file).ok());
  const InodeID f_ino = file.ino;
  ASSERT_TRUE(impl_->Unlink(kRoot, "f").ok());
  EXPECT_EQ(impl_->PrepareReclaim(f_ino, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(OrphanCandidates(), std::vector<InodeID>{f_ino});
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(MemMetaImplTest, VisitorAbortStopsTheScanAndIsPropagated) {
  SetContext(0, 0);

  // Two candidates, so a stop is observable: the first visited inode ends the
  // walk and the caller sees the visitor's own status. The remaining candidate
  // stays published for the next reconciliation pass — an aborted scan must
  // never consume state.
  SwordFsInode first_file;
  SwordFsInode second_file;
  ASSERT_TRUE(impl_->Create(kRoot, "a", 0644, &first_file).ok());
  ASSERT_TRUE(impl_->Create(kRoot, "b", 0644, &second_file).ok());
  const InodeID first = first_file.ino;
  const InodeID second = second_file.ino;
  ASSERT_TRUE(impl_->Unlink(kRoot, "a").ok());
  ASSERT_TRUE(impl_->Unlink(kRoot, "b").ok());
  ASSERT_EQ(OrphanCandidates(), (std::vector<InodeID>{first, second}));

  std::vector<InodeID> visited;
  const auto orphan_status = impl_->VisitOrphanCandidates([&](InodeID ino) {
    visited.push_back(ino);
    return Status::IOError("abort the orphan scan");
  });
  EXPECT_EQ(orphan_status.ToErrno(), EIO);
  EXPECT_EQ(orphan_status.message(), "abort the orphan scan");
  EXPECT_EQ(visited, (std::vector<InodeID>{first}));
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{first, second}));

  // The same contract holds for the pending-reclaim scan.
  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(second, &work).ok());
  ASSERT_TRUE(work.has_value());
  ASSERT_EQ(PendingReclaims(), (std::vector<InodeID>{second}));

  visited.clear();
  const auto pending_status = impl_->VisitPendingReclaims([&](const ReclaimWork &work) {
    visited.push_back(work.ino);
    return Status::Busy("abort the pending scan");
  });
  EXPECT_EQ(pending_status.ToErrno(), EBUSY);
  EXPECT_EQ(pending_status.message(), "abort the pending scan");
  EXPECT_EQ(visited, (std::vector<InodeID>{second}));
}

FIBER_TEST_F(MemMetaImplTest, PendingDeleteBatchVisitorAbortIsPropagated) {
  SetContext(0, 0);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "truncate-pending", 0644, &file).ok());
  const InodeID ino = file.ino;
  ASSERT_TRUE(impl_->CommitChunk(ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());
  ASSERT_TRUE(impl_->Truncate(ino, 0).ok());

  size_t visits = 0;
  bool has_more = true;
  const auto status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &) {
        ++visits;
        return Status::Busy("abort the pending delete scan");
      },
      &has_more);
  EXPECT_EQ(status.ToErrno(), EBUSY);
  EXPECT_EQ(status.message(), "abort the pending delete scan");
  EXPECT_EQ(visits, 1U);
  EXPECT_FALSE(has_more);
}

FIBER_TEST_F(MemMetaImplTest, PendingDeleteBatchBoundsVisitsAndValidatesArguments) {
  SetContext(0, 0);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "truncate-pending-batch", 0644, &file).ok());
  const InodeID ino = file.ino;
  ASSERT_TRUE(impl_->CommitChunk(ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());
  ASSERT_TRUE(impl_->CommitChunk(ino, std::nullopt, SwordFsChunk{.index = 1, .revision = 2, .size = 64}).ok());
  ASSERT_TRUE(impl_->Truncate(ino, 0).ok());

  bool has_more = false;
  size_t visits = 0;
  auto status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &) {
        ++visits;
        return Status::OK();
      },
      &has_more);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(visits, 1U);
  EXPECT_TRUE(has_more);

  visits = 0;
  status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &) {
        ++visits;
        return Status::Busy("abort bounded pending delete scan");
      },
      &has_more);
  EXPECT_TRUE(status.ToErrno() == EBUSY) << status.message();
  EXPECT_EQ(status.message(), "abort bounded pending delete scan");
  EXPECT_EQ(visits, 1U);
  EXPECT_FALSE(has_more);

  EXPECT_EQ(impl_->VisitPendingDeletesBatch(0, [](const auto &) { return Status::OK(); }, &has_more).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, swordfs::metadata::PendingDeleteVisitorFn{}, &has_more).ToErrno(),
            EINVAL);
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, [](const auto &) { return Status::OK(); }, nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(MemMetaImplTest, PendingDeleteBatchMakesProgressWithoutQueueMutation) {
  SetContext(0, 0);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "truncate-pending-progress", 0644, &file).ok());
  const InodeID ino = file.ino;
  ASSERT_TRUE(impl_->CommitChunk(ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());
  ASSERT_TRUE(impl_->CommitChunk(ino, std::nullopt, SwordFsChunk{.index = 1, .revision = 2, .size = 64}).ok());
  ASSERT_TRUE(impl_->Truncate(ino, 0).ok());

  std::vector<std::string> visited;
  bool has_more = false;
  auto visit_one = [&](const swordfs::metadata::PendingDelete &work) {
    visited.push_back(work.id);
    return Status::OK();
  };

  ASSERT_TRUE(impl_->VisitPendingDeletesBatch(1, visit_one, &has_more).ok());
  ASSERT_TRUE(has_more);
  ASSERT_EQ(visited.size(), 1U);

  ASSERT_TRUE(impl_->VisitPendingDeletesBatch(1, visit_one, &has_more).ok());
  EXPECT_FALSE(has_more);
  ASSERT_EQ(visited.size(), 2U);
  EXPECT_NE(visited[0], visited[1]);
}
