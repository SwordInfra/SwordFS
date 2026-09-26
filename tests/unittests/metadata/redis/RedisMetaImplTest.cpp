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
#include "runtime/MountRuntimeBehavior.hpp"

namespace {

using swordfs::test::redis_meta::InodeID;
using swordfs::test::redis_meta::kRootInodeId;
using swordfs::test::redis_meta::kTestChunkSize;
using swordfs::test::redis_meta::MakePendingDelete;
using swordfs::test::redis_meta::PendingDeleteObjectKey;
using swordfs::test::redis_meta::ReclaimWork;
using swordfs::test::redis_meta::RedisMetaConfig;
using swordfs::test::redis_meta::RedisMetaImpl;
using swordfs::test::redis_meta::RedisMetaImplTest;
using swordfs::test::redis_meta::SetAttrField;
using swordfs::test::redis_meta::Status;
using swordfs::test::redis_meta::SwordFsAttr;
using swordfs::test::redis_meta::SwordFsChunk;
using swordfs::test::redis_meta::SwordFsContext;
using swordfs::test::redis_meta::SwordFsEntry;
using swordfs::test::redis_meta::SwordFsInode;
using swordfs::test::redis_meta::SwordFsVolume;

FIBER_TEST_F(RedisMetaImplTest, AllocateChunkRevisionIsMonotonicAndStartsAtOne) {
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

  std::unique_ptr<RedisMetaImpl> peer;
  swordfs::test::RunInTestThreadFromFiber([&] {
    peer = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    ASSERT_TRUE(peer->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    ASSERT_TRUE(peer->LoadVolume(&volume).ok());
  });

  swordfs::metadata::ChunkRevision peer_revision = 0;
  ASSERT_TRUE(peer->AllocateChunkRevision(&peer_revision).ok());
  EXPECT_EQ(peer_revision, 4U);
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, OpenDirReturnsIndependentIteratorsAndSupportsSeek) {
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "second", 0644, &second).ok());

  swordfs::metadata::DirIteratorPtr first_iterator;
  swordfs::metadata::DirIteratorPtr second_iterator;
  ASSERT_TRUE(impl_->OpenDir(kRootInodeId, &first_iterator).ok());
  ASSERT_TRUE(impl_->OpenDir(kRootInodeId, &second_iterator).ok());
  ASSERT_NE(first_iterator, nullptr);
  ASSERT_NE(second_iterator, nullptr);

  SwordFsEntry entry;
  uint64_t next_offset = 0;
  EXPECT_EQ(first_iterator->Peek(nullptr, &next_offset).ToErrno(), EINVAL);
  EXPECT_EQ(first_iterator->Peek(&entry, nullptr).ToErrno(), EINVAL);
  ASSERT_TRUE(first_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, ".");
  EXPECT_EQ(next_offset, 1);
  EXPECT_EQ(first_iterator->Peek(&entry, &next_offset).ToErrno(), EINVAL);
  first_iterator->Advance();
  ASSERT_TRUE(first_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, "..");
  EXPECT_EQ(next_offset, 2);
  first_iterator->Advance();
  ASSERT_TRUE(first_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(next_offset, 3);
  first_iterator->Advance();

  // Seeking behind the iterator's evicted window rebuilds from HSCAN cursor 0.
  ASSERT_TRUE(first_iterator->Seek(0).ok());
  ASSERT_TRUE(first_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, ".");
  EXPECT_EQ(next_offset, 1);

  // Each OpenDir owns independent HSCAN state, so the second iterator still
  // starts from its own position regardless of the first iterator's progress.
  ASSERT_TRUE(second_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, ".");
  EXPECT_EQ(next_offset, 1);
  second_iterator->Advance();

  ASSERT_TRUE(second_iterator->Seek(1).ok());
  ASSERT_TRUE(second_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, "..");
  EXPECT_EQ(next_offset, 2);
  second_iterator->Advance();

  ASSERT_TRUE(second_iterator->Seek(2).ok());
  ASSERT_TRUE(second_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, "first");
  EXPECT_EQ(next_offset, 3);
  second_iterator->Advance();
}

FIBER_TEST_F(RedisMetaImplTest, GetInodesReturnsAlignedPresentAndMissingResults) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "batch-file", 0644, &file).ok());

  std::vector<std::optional<SwordFsInode>> results;
  ASSERT_TRUE(impl_->GetInodes({file.ino, 999999}, &results).ok());
  ASSERT_EQ(results.size(), 2u);
  ASSERT_TRUE(results[0].has_value());
  EXPECT_EQ(results[0]->ino, file.ino);
  EXPECT_FALSE(results[1].has_value());
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentOpenDoesNotFailOnAtimeContention) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  constexpr size_t kThreadCount = 8;
  const InodeID file_ino = file.ino;
  std::vector<std::thread> threads;
  std::vector<Status> statuses(kThreadCount);
  threads.reserve(kThreadCount);
  for (size_t i = 0; i < kThreadCount; ++i) {
    threads.push_back(swordfs::test::StartFiberTestThread([this, &statuses, file_ino, i] {
      folly::fibers::local<SwordFsContext>() = SwordFsContext{};
      statuses[i] = impl_->Open(file_ino);
    }));
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (const auto &status : statuses) {
    EXPECT_TRUE(status.ok()) << status.message();
  }
}

TEST_F(RedisMetaImplTest, NoAtimeRuntimeBehaviorSuppressesImplicitOpenUpdates) {
  swordfs::runtime::MountRuntimeBehavior::Instance().Initialize(swordfs::runtime::ImplicitAtimePolicy::kDisabled);

  swordfs::test::RunInTestFiber([&] {
    SwordFsInode file;
    ASSERT_TRUE(impl_->Create(kRootInodeId, "noatime-file", 0644, &file).ok());
    SwordFsInode dir;
    ASSERT_TRUE(impl_->MkDir(kRootInodeId, "noatime-dir", 0755, &dir).ok());

    SwordFsAttr requested;
    requested.atime = 11;
    ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kAtime, nullptr).ok());
    ASSERT_TRUE(impl_->Open(file.ino).ok());
    SwordFsInode actual;
    ASSERT_TRUE(impl_->GetInode(file.ino, &actual).ok());
    EXPECT_EQ(actual.attr.atime, 11);

    requested.atime = 21;
    ASSERT_TRUE(impl_->SetAttr(dir.ino, requested, SetAttrField::kAtime, nullptr).ok());
    swordfs::metadata::DirIteratorPtr iterator;
    ASSERT_TRUE(impl_->OpenDir(dir.ino, &iterator).ok());
    ASSERT_NE(iterator, nullptr);
    ASSERT_TRUE(impl_->GetInode(dir.ino, &actual).ok());
    EXPECT_EQ(actual.attr.atime, 21);

    requested.atime = 31;
    ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kAtime, nullptr).ok());
    ASSERT_TRUE(impl_->GetInode(file.ino, &actual).ok());
    EXPECT_EQ(actual.attr.atime, 31);
  });

  swordfs::runtime::MountRuntimeBehavior::Instance().Initialize(swordfs::runtime::ImplicitAtimePolicy::kEnabled);
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrPreservesExplicitCtime) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  SwordFsAttr requested = file.attr;
  requested.ctime = 123;
  requested.ctime_nsec = 456;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kCtime, nullptr).ok());

  SwordFsInode actual;
  ASSERT_TRUE(impl_->GetInode(file.ino, &actual).ok());
  EXPECT_EQ(actual.attr.ctime, requested.ctime);
  EXPECT_EQ(actual.attr.ctime_nsec, requested.ctime_nsec);
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrSameOwnerKeepsSuidSgid) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0755, &file).ok());

  SwordFsAttr requested = file.attr;
  requested.mode = S_IFREG | 06755;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kMode, &file).ok());
  ASSERT_NE(file.attr.mode & (S_ISUID | S_ISGID), 0U);

  requested = file.attr;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kUid, &file).ok());
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), requested.mode & (S_ISUID | S_ISGID));
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrShrinkRemovesAndClampsChunks) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  SwordFsAttr initial = file.attr;
  initial.size = 8192;
  ASSERT_TRUE(impl_->SetAttr(file.ino, initial, SetAttrField::kSize, &file).ok());

  SwordFsChunk chunk0;
  chunk0.index = 0;
  chunk0.revision = 1;
  chunk0.size = 4096;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk0).ok());

  SwordFsChunk chunk1;
  chunk1.index = 1;
  chunk1.revision = 2;
  chunk1.size = 4096;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk1).ok());

  SwordFsAttr requested = file.attr;
  requested.size = 100;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr).ok());

  SwordFsChunk actual;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &actual).ok());
  EXPECT_EQ(actual.size, 100U);
  EXPECT_TRUE(impl_->FindChunk(file.ino, 1, &actual).IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, VolumeAndBasicLookupOperations) {
  SwordFsVolume volume;
  volume.name = volume_name_;
  ASSERT_TRUE(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->LoadVolume(&volume); }).ok());
  EXPECT_EQ(volume.name, volume_name_);
  EXPECT_EQ(volume.chunk_size, 4096U);
  EXPECT_EQ(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->LoadVolume(nullptr); }).ToErrno(), EINVAL);
  EXPECT_TRUE(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->FormatVolume(volume); }).ToErrno() == EEXIST);

  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  EXPECT_EQ(root.attr.mode & 0777u, 0755u);

  const auto limits = impl_->GetLimits();
  EXPECT_EQ(limits.max_name_length, 255U);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_EQ(file.ino, kRootInodeId + 1);
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "missing", &found).IsNotFound());
  EXPECT_EQ(impl_->Lookup(kRootInodeId, "file", nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->GetInode(file.ino, nullptr).ToErrno(), EINVAL);
  EXPECT_TRUE(impl_->Lookup(file.ino, "child", &found).ToErrno() == ENOTDIR);
}

FIBER_TEST_F(RedisMetaImplTest, NamespaceOperationsRejectOverlongNameComponents) {
  const auto limits = impl_->GetLimits();
  const std::string long_name(limits.max_name_length + 1, 'x');

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, long_name, &found).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, long_name).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, long_name).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, long_name, kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      ENAMETOOLONG);
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, long_name, swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      ENAMETOOLONG);

  EXPECT_TRUE(impl_->Create(kRootInodeId, long_name, 0644, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, long_name, 0755, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, long_name, "target", nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, long_name, nullptr).ToErrno() == ENAMETOOLONG);
}

FIBER_TEST_F(RedisMetaImplTest, MknodPersistsSupportedTypesModeAndDeviceIdentity) {
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
    ASSERT_TRUE(impl_->MkNod(kRootInodeId, test_case.name, test_case.mode, test_case.rdev, &created).ok())
        << test_case.name;
    EXPECT_EQ(created.attr.mode, static_cast<uint32_t>(test_case.mode)) << test_case.name;
    EXPECT_EQ(created.attr.rdev, static_cast<uint64_t>(test_case.expected_rdev)) << test_case.name;

    SwordFsInode found;
    ASSERT_TRUE(impl_->Lookup(kRootInodeId, test_case.name, &found).ok()) << test_case.name;
    EXPECT_EQ(found.attr.mode, created.attr.mode) << test_case.name;
    EXPECT_EQ(found.attr.rdev, created.attr.rdev) << test_case.name;
  }
}

FIBER_TEST_F(RedisMetaImplTest, CreateOwnershipUsesCallerGidWithoutParentSgid) {
  constexpr uid_t kCallerUid = 4101;
  constexpr gid_t kCallerGid = 4102;
  constexpr gid_t kParentGid = 5102;

  SwordFsInode parent;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "ordinary-parent", 0755, &parent).ok());
  SwordFsAttr parent_attr = parent.attr;
  parent_attr.gid = kParentGid;
  ASSERT_TRUE(impl_->SetAttr(parent.ino, parent_attr, SetAttrField::kGid, &parent).ok());

  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx.uid = kCallerUid;
  ctx.gid = kCallerGid;

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(parent.ino, "file", 0644, &file).ok());
  EXPECT_EQ(file.attr.uid, kCallerUid);
  EXPECT_EQ(file.attr.gid, kCallerGid);

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(parent.ino, "dir", 0755, &dir).ok());
  EXPECT_EQ(dir.attr.uid, kCallerUid);
  EXPECT_EQ(dir.attr.gid, kCallerGid);
  EXPECT_EQ(dir.attr.mode & S_ISGID, 0U);

  SwordFsInode fifo;
  ASSERT_TRUE(impl_->MkNod(parent.ino, "fifo", S_IFIFO | 0600, 0, &fifo).ok());
  EXPECT_EQ(fifo.attr.gid, kCallerGid);

  SwordFsInode symlink;
  ASSERT_TRUE(impl_->Symlink(parent.ino, "symlink", "target", &symlink).ok());
  EXPECT_EQ(symlink.attr.gid, kCallerGid);
}

FIBER_TEST_F(RedisMetaImplTest, CreateOwnershipInheritsGidAndDirectorySgidFromParent) {
  constexpr uid_t kCallerUid = 4201;
  constexpr gid_t kCallerGid = 4202;
  constexpr gid_t kParentGid = 5202;

  SwordFsInode parent;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "sgid-parent", 0755, &parent).ok());
  SwordFsAttr parent_attr = parent.attr;
  parent_attr.gid = kParentGid;
  ASSERT_TRUE(impl_->SetAttr(parent.ino, parent_attr, SetAttrField::kGid, &parent).ok());
  parent_attr = parent.attr;
  parent_attr.mode = S_IFDIR | 02775;
  ASSERT_TRUE(impl_->SetAttr(parent.ino, parent_attr, SetAttrField::kMode, &parent).ok());

  auto &ctx = folly::fibers::local<SwordFsContext>();
  ctx.uid = kCallerUid;
  ctx.gid = kCallerGid;

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(parent.ino, "file", 0644, &file).ok());
  EXPECT_EQ(file.attr.uid, kCallerUid);
  EXPECT_EQ(file.attr.gid, kParentGid);

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(parent.ino, "dir", 0755, &dir).ok());
  EXPECT_EQ(dir.attr.uid, kCallerUid);
  EXPECT_EQ(dir.attr.gid, kParentGid);
  EXPECT_NE(dir.attr.mode & S_ISGID, 0U);

  SwordFsInode fifo;
  ASSERT_TRUE(impl_->MkNod(parent.ino, "fifo", S_IFIFO | 0600, 0, &fifo).ok());
  EXPECT_EQ(fifo.attr.gid, kParentGid);

  SwordFsInode symlink;
  ASSERT_TRUE(impl_->Symlink(parent.ino, "symlink", "target", &symlink).ok());
  EXPECT_EQ(symlink.attr.gid, kParentGid);
}

FIBER_TEST_F(RedisMetaImplTest, MknodReusesNamespaceValidationAndRejectsNonMknodTypes) {
  const std::string long_name(impl_->GetLimits().max_name_length + 1, 'x');
  EXPECT_TRUE(impl_->MkNod(kRootInodeId, long_name, S_IFIFO | 0600, 0, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_EQ(impl_->MkNod(kRootInodeId, "directory", S_IFDIR | 0700, 0, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->MkNod(kRootInodeId, "symlink", S_IFLNK | 0700, 0, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->MkNod(kRootInodeId, "unknown", 0700, 0, nullptr).ToErrno(), EINVAL);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "parent-file", 0644, &file).ok());
  EXPECT_TRUE(impl_->MkNod(file.ino, "child", S_IFIFO | 0600, 0, nullptr).ToErrno() == ENOTDIR);
  EXPECT_TRUE(impl_->MkNod(999999, "missing-parent", S_IFIFO | 0600, 0, nullptr).IsNotFound());

  ASSERT_TRUE(impl_->MkNod(kRootInodeId, "duplicate", S_IFIFO | 0600, 0, nullptr).ok());
  EXPECT_TRUE(impl_->MkNod(kRootInodeId, "duplicate", S_IFIFO | 0600, 0, nullptr).ToErrno() == EEXIST);
}

FIBER_TEST_F(RedisMetaImplTest, MknodSpecialNodesUseOrdinaryNamespaceLifecycle) {
  SwordFsInode fifo;
  ASSERT_TRUE(impl_->MkNod(kRootInodeId, "fifo", S_IFIFO | 0600, 0, &fifo).ok());

  swordfs::metadata::DirIteratorPtr iterator;
  ASSERT_TRUE(impl_->OpenDir(kRootInodeId, &iterator).ok());
  bool found_fifo = false;
  for (;;) {
    SwordFsEntry entry;
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

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "fifo").ok());
  std::vector<InodeID> orphans;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&](InodeID ino) {
                    orphans.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(orphans, std::vector<InodeID>{fifo.ino});

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(fifo.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kTestChunkSize, &refs).ok());
  EXPECT_TRUE(refs.empty());
  SwordFsInode reclaimed;
  EXPECT_TRUE(impl_->GetInode(fifo.ino, &reclaimed).IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, CreateAndMkdirValidateNamesParentsAndDuplicates) {
  const std::string long_name(256, 'x');
  EXPECT_TRUE(impl_->Create(kRootInodeId, long_name, 0644, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, long_name, 0755, nullptr).ToErrno() == ENAMETOOLONG);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_TRUE(impl_->Create(kRootInodeId, "file", 0644, nullptr).ToErrno() == EEXIST);
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "file", 0755, nullptr).ToErrno() == EEXIST);
  EXPECT_TRUE(impl_->Create(file.ino, "child", 0644, nullptr).ToErrno() == ENOTDIR);
  EXPECT_TRUE(impl_->MkDir(file.ino, "child", 0755, nullptr).ToErrno() == ENOTDIR);
  EXPECT_TRUE(impl_->Create(999999, "child", 0644, nullptr).IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, UnlinkAndRmdirCoverSuccessAndTypeChecks) {
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());

  EXPECT_EQ(impl_->Unlink(kRootInodeId, ".").ToErrno(), EINVAL);
  EXPECT_EQ(impl_->RmDir(kRootInodeId, "..").ToErrno(), EINVAL);
  EXPECT_EQ(impl_->Unlink(kRootInodeId, "dir").ToErrno(), EINVAL);
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "file").ToErrno() == ENOTDIR);

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &file).IsNotFound());
  std::vector<InodeID> orphans;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&](InodeID ino) {
                    orphans.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(orphans, std::vector<InodeID>{file.ino});
  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  EXPECT_EQ(work->ino, file.ino);
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kTestChunkSize, &refs).ok());
  EXPECT_TRUE(refs.empty());
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());

  SwordFsInode child;
  ASSERT_TRUE(impl_->Create(dir.ino, "child", 0644, &child).ok());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").ToErrno() == ENOTEMPTY);
  ASSERT_TRUE(impl_->Unlink(dir.ino, "child").ok());
  ASSERT_TRUE(impl_->RmDir(kRootInodeId, "dir").ok());
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrAndStatFsCoverCommonFields) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 06755, &file).ok());

  SwordFsAttr requested = file.attr;
  requested.mode = S_IFREG | 0600;
  requested.uid = 123;
  requested.gid = 456;
  requested.atime = 11;
  requested.atime_nsec = 12;
  requested.mtime = 21;
  requested.mtime_nsec = 22;
  ASSERT_TRUE(impl_
                  ->SetAttr(file.ino, requested,
                            SetAttrField::kMode | SetAttrField::kUid | SetAttrField::kGid | SetAttrField::kAtime |
                                SetAttrField::kMtime,
                            &file)
                  .ok());
  EXPECT_EQ(file.attr.mode & 07777U, 0600U);
  EXPECT_EQ(file.attr.uid, 123U);
  EXPECT_EQ(file.attr.gid, 456U);
  EXPECT_EQ(file.attr.atime, 11);
  EXPECT_EQ(file.attr.mtime, 21);
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), 0U);

  swordfs::metadata::SwordFsStatFs stat;
  ASSERT_TRUE(impl_->StatFs(&stat).ok());
  EXPECT_EQ(stat.files, impl_->GetLimits().max_free_inodes);
  EXPECT_EQ(stat.files_free, stat.files);
  EXPECT_GT(stat.files, 0U);
  EXPECT_EQ(stat.name_max, impl_->GetLimits().max_name_length);
  EXPECT_EQ(impl_->StatFs(nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(RedisMetaImplTest, SymlinkHardLinkAndOpenBehaveLikePosixMetadata) {
  SwordFsInode link;
  ASSERT_TRUE(impl_->Symlink(kRootInodeId, "link", "target/path", &link).ok());
  std::string target;
  ASSERT_TRUE(impl_->Readlink(link.ino, &target).ok());
  EXPECT_EQ(target, "target/path");
  EXPECT_EQ(impl_->Readlink(link.ino, nullptr).ToErrno(), EINVAL);
  EXPECT_TRUE(impl_->Open(link.ino).ToErrno() == ENOTDIR);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_EQ(impl_->Readlink(file.ino, &target).ToErrno(), EINVAL);
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", &file).ok());
  EXPECT_EQ(file.attr.nlink, 2U);
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).ToErrno() == EEXIST);
  EXPECT_TRUE(impl_->Link(link.ino, kRootInodeId, "hard-link", nullptr).ok());

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  EXPECT_TRUE(impl_->Link(dir.ino, kRootInodeId, "dir-hard", nullptr).ToErrno() == EPERM);
  uint64_t open_size = 1;
  EXPECT_TRUE(impl_->Open(file.ino, &open_size).ok());
  EXPECT_EQ(open_size, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, ChunkFindAndTruncateCoverSparseMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 9000).ok());

  SwordFsChunk chunk0{.index = 0, .revision = 1, .size = 4096};
  SwordFsChunk chunk2{.index = 2, .revision = 2, .size = 808};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk0).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk2).ok());

  SwordFsChunk found;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 2, &found).ok());
  EXPECT_EQ(found.index, 2U);
  EXPECT_EQ(impl_->FindChunk(file.ino, 2, nullptr).ToErrno(), EINVAL);

  ASSERT_TRUE(impl_->Truncate(file.ino, 4096).ok());
  EXPECT_TRUE(impl_->FindChunk(file.ino, 2, &found).IsNotFound());
  ASSERT_TRUE(impl_->Truncate(file.ino, 0).ok());
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &found).IsNotFound());
  ASSERT_TRUE(impl_->Truncate(file.ino, 0).ok());
}

FIBER_TEST_F(RedisMetaImplTest, LoadChunkViewValidatesWholeObjectSnapshot) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "chunk-view", 0644, &file).ok());
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

FIBER_TEST_F(RedisMetaImplTest, SetAttrShrinkQueuesOnlyMaterializedSparseObjectsForCleanup) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "sparse-cleanup", 0644, &file).ok());

  constexpr swordfs::metadata::ChunkIndex kFarIndex = 1000000000U;
  constexpr uint64_t kChunkSize = 4096;
  SwordFsChunk head{.index = 0, .revision = 1, .size = 128};
  SwordFsChunk far{.index = kFarIndex, .revision = 2, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, head).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, far).ok());

  SwordFsAttr requested = file.attr;
  requested.size = 1;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr).ok());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored.size, 1U);
  EXPECT_TRUE(impl_->FindChunk(file.ino, kFarIndex, &stored).IsNotFound());

  const auto far_key = swordfs::chunk::FormatChunkObjectKey(file.ino, kFarIndex, far.revision);
  EXPECT_EQ(PendingDeleteKeys(), std::vector<std::string>{far_key});

  // A fresh metadata-engine instance sees the same durable queue, modelling
  // remount/restart before object deletion.
  std::unique_ptr<RedisMetaImpl> peer;
  swordfs::test::RunInTestThreadFromFiber([&] {
    peer = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    ASSERT_TRUE(peer->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    ASSERT_TRUE(peer->LoadVolume(&volume).ok());
  });
  EXPECT_EQ(PendingDeleteKeys(peer.get()), std::vector<std::string>{far_key});
  ASSERT_TRUE(peer->CompletePendingDelete("whole_object:" + far_key).ok());
  EXPECT_TRUE(PendingDeleteKeys(peer.get()).empty());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ChunkAndOpenOperationsRejectWrongInodeTypes) {
  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 1};
  EXPECT_EQ(impl_->CommitChunk(dir.ino, std::nullopt, chunk).ToErrno(), EINVAL);
  EXPECT_TRUE(impl_->Open(dir.ino).ToErrno() == ENOTDIR);
  EXPECT_TRUE(impl_->OpenDir(dir.ino, nullptr).ToErrno() == EINVAL);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  swordfs::metadata::DirIteratorPtr iterator;
  EXPECT_TRUE(impl_->OpenDir(file.ino, &iterator).ToErrno() == ENOTDIR);
}

FIBER_TEST_F(RedisMetaImplTest, MetadataDoesNotDuplicateKernelDac) {
  SwordFsAttr root_attr;
  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  root_attr = root.attr;
  root_attr.mode = S_IFDIR | 01000;
  ASSERT_TRUE(impl_->SetAttr(kRootInodeId, root_attr, SetAttrField::kMode, nullptr).ok());

  SwordFsContext ctx;
  ctx.uid = 1000;
  ctx.gid = 1000;
  folly::fibers::local<SwordFsContext>() = ctx;

  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0000, &file).ok());
  ASSERT_TRUE(impl_->Open(file.ino).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0000, &dir).ok());
  ASSERT_TRUE(impl_->Symlink(kRootInodeId, "link", "target", nullptr).ok());
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).ok());
  ASSERT_TRUE(impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "moved").ok());
  ASSERT_TRUE(impl_->RmDir(kRootInodeId, "dir").ok());
}

FIBER_TEST_F(RedisMetaImplTest, StickyDirectoryOwnershipSafetyRemainsInMetadata) {
  SwordFsInode sticky_unlink;
  SwordFsInode sticky_rmdir;
  SwordFsInode sticky_source;
  SwordFsInode plain_dest;
  SwordFsInode plain_source;
  SwordFsInode sticky_dest;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "sticky-unlink", 01777, &sticky_unlink).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "sticky-rmdir", 01777, &sticky_rmdir).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "sticky-source", 01777, &sticky_source).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "plain-dest", 0777, &plain_dest).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "plain-source", 0777, &plain_source).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "sticky-dest", 01777, &sticky_dest).ok());

  SwordFsAttr sticky_attr;
  sticky_attr.uid = 1000;
  sticky_attr.mode = S_IFDIR | 01777;
  const auto sticky_fields = SetAttrField::kUid | SetAttrField::kMode;
  ASSERT_TRUE(impl_->SetAttr(sticky_unlink.ino, sticky_attr, sticky_fields, nullptr).ok());
  ASSERT_TRUE(impl_->SetAttr(sticky_rmdir.ino, sticky_attr, sticky_fields, nullptr).ok());
  ASSERT_TRUE(impl_->SetAttr(sticky_source.ino, sticky_attr, sticky_fields, nullptr).ok());
  ASSERT_TRUE(impl_->SetAttr(sticky_dest.ino, sticky_attr, sticky_fields, nullptr).ok());

  ASSERT_TRUE(impl_->Create(sticky_unlink.ino, "file", 0644, nullptr).ok());
  ASSERT_TRUE(impl_->MkDir(sticky_rmdir.ino, "subdir", 0755, nullptr).ok());
  ASSERT_TRUE(impl_->Create(sticky_source.ino, "source", 0644, nullptr).ok());
  ASSERT_TRUE(impl_->Create(plain_source.ino, "source", 0644, nullptr).ok());
  ASSERT_TRUE(impl_->Create(sticky_dest.ino, "target", 0644, nullptr).ok());

  SwordFsContext ctx;
  ctx.uid = 2000;
  ctx.gid = 2000;
  folly::fibers::local<SwordFsContext>() = ctx;

  EXPECT_TRUE(impl_->Unlink(sticky_unlink.ino, "file").ToErrno() == EACCES);
  EXPECT_TRUE(impl_->RmDir(sticky_rmdir.ino, "subdir").ToErrno() == EACCES);
  EXPECT_TRUE(impl_->Rename(sticky_source.ino, "source", plain_dest.ino, "moved", swordfs::metadata::RenameFlag::kNone)
                  .ToErrno() == EACCES);
  EXPECT_TRUE(impl_->Rename(plain_source.ino, "source", sticky_dest.ino, "target", swordfs::metadata::RenameFlag::kNone)
                  .ToErrno() == EACCES);
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrNowAndGrowPreserveExpectedMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 06755, &file).ok());
  const auto old_atime = file.attr.atime;
  const auto old_mtime = file.attr.mtime;

  SwordFsAttr requested = file.attr;
  requested.size = 8192;
  ASSERT_TRUE(
      impl_
          ->SetAttr(file.ino, requested, SetAttrField::kSize | SetAttrField::kAtimeNow | SetAttrField::kMtimeNow, &file)
          .ok());
  EXPECT_EQ(file.attr.size, 8192U);
  EXPECT_GE(file.attr.atime, old_atime);
  EXPECT_GE(file.attr.mtime, old_mtime);
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), 0U);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkInitialPublishIsIdempotentAndGrowsSizeMonotonically) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "publish", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .revision = 1, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 128U);

  SwordFsChunk later{.index = 2, .revision = 2, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, later).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 8256U);

  auto conflicting = first;
  conflicting.revision = 3;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, conflicting).ToErrno() == EEXIST);
  EXPECT_EQ(PendingDeleteKeys(), std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(
                                     file.ino, conflicting.index, conflicting.revision)});

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored.revision, first.revision);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkRewriteUsesCompareAndSwapAndIsIdempotent) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .revision = 1, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  SwordFsAttr mode{};
  mode.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, mode, SetAttrField::kMode, nullptr).ok());

  auto replacement = first;
  replacement.revision = 2;
  replacement.size = 64;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());
  EXPECT_EQ(PendingDeleteKeys(),
            std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision)});
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, replacement);

  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 128U);
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), static_cast<uint32_t>(S_ISUID | S_ISGID));

  auto stale_replacement = replacement;
  stale_replacement.revision = 3;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, first, stale_replacement).ToErrno() == EEXIST);
  EXPECT_EQ(PendingDeleteKeys(),
            (std::vector<std::string>{
                swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision),
                swordfs::chunk::FormatChunkObjectKey(file.ino, stale_replacement.index, stale_replacement.revision)}));

  auto grown = replacement;
  grown.revision = 4;
  grown.size = 256;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, replacement, grown).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 256U);
}

FIBER_TEST_F(RedisMetaImplTest, RewritePendingDeleteSurvivesMetadataRemount) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "rewrite-remount", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  auto replacement = first;
  replacement.revision = 2;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());

  const auto old_key = swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision);
  EXPECT_EQ(PendingDeleteKeys(), std::vector<std::string>{old_key});

  std::unique_ptr<RedisMetaImpl> peer;
  swordfs::test::RunInTestThreadFromFiber([&] {
    peer = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    ASSERT_TRUE(peer->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    ASSERT_TRUE(peer->LoadVolume(&volume).ok());
  });

  EXPECT_EQ(PendingDeleteKeys(peer.get()), std::vector<std::string>{old_key});
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, RewriteCleanupRegistrationFailureDoesNotInvalidatePublication) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "rewrite-preflight", 0644, &file).ok());
  SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.PendingDeletes(), "wrong-type"); });

  auto replacement = first;
  replacement.revision = 2;
  const auto status = impl_->CommitChunk(file.ino, first, replacement);
  EXPECT_TRUE(status.ok()) << status.message();

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, replacement);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkReplayRepairsInodeAfterPartialExec) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace-repair", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  SwordFsAttr mode{};
  mode.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, mode, SetAttrField::kMode, &file).ok());
  ASSERT_NE(file.attr.mode & (S_ISUID | S_ISGID), 0U);
  ASSERT_EQ(file.attr.size, 64U);

  auto replacement = first;
  replacement.revision = 2;
  replacement.size = 128;
  std::string replacement_value;
  ASSERT_TRUE(replacement.SerializeTo(&replacement_value).ok());

  // Simulate an EXEC where SetChunk succeeded but the later SetInode command
  // failed. The retry sees replacement metadata already installed and must
  // still repair the inode size/timestamps instead of returning early.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hset(key.Chunk(file.ino), std::to_string(first.index), replacement_value);
  });

  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());
  EXPECT_EQ(PendingDeleteKeys(),
            std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision)});
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 128U);
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), static_cast<uint32_t>(S_ISUID | S_ISGID));

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, replacement);
}

FIBER_TEST_F(RedisMetaImplTest, SizePreservingTruncateReconcilesDescriptorAfterPartialReplace) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace-truncate-repair", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .revision = 1, .size = 5};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  ASSERT_EQ(file.attr.size, 5U);

  auto replacement = first;
  replacement.revision = 2;
  replacement.size = 11;
  std::string replacement_value;
  ASSERT_TRUE(replacement.SerializeTo(&replacement_value).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto install_partial_replace = [&] {
    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      redis.hset(key.Chunk(file.ino), std::to_string(first.index), replacement_value);
    });
  };

  install_partial_replace();
  SwordFsAttr requested = file.attr;
  requested.size = 5;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr).ok());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored.revision, replacement.revision);
  EXPECT_EQ(stored.size, 5U);

  install_partial_replace();
  ASSERT_TRUE(impl_->Truncate(file.ino, 5).ok());
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored.revision, replacement.revision);
  EXPECT_EQ(stored.size, 5U);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkRejectsInvalidTargetsAndDescriptors) {
  SwordFsChunk expected{.index = 0, .revision = 1, .size = 128};
  auto replacement = expected;
  replacement.revision = 2;

  auto invalid_revision = expected;
  invalid_revision.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_EQ(impl_->CommitChunk(999999, invalid_revision, replacement).ToErrno(), EINVAL);

  auto invalid_replacement = replacement;
  invalid_replacement.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_EQ(impl_->CommitChunk(999999, expected, invalid_replacement).ToErrno(), EINVAL);

  auto oversized_replacement = replacement;
  oversized_replacement.size = 4097;
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, oversized_replacement).ToErrno(), EINVAL);

  EXPECT_TRUE(impl_->CommitChunk(999999, expected, replacement).IsNotFound());

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "replace-invalid-dir", 0755, &dir).ok());
  EXPECT_EQ(impl_->CommitChunk(dir.ino, expected, replacement).ToErrno(), EINVAL);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace-invalid-file", 0644, &file).ok());
  EXPECT_TRUE(impl_->CommitChunk(file.ino, expected, replacement).IsNotFound());
  std::vector<std::string> expected_pending{
      swordfs::chunk::FormatChunkObjectKey(999999, replacement.index, replacement.revision),
      swordfs::chunk::FormatChunkObjectKey(file.ino, replacement.index, replacement.revision)};
  std::sort(expected_pending.begin(), expected_pending.end());
  EXPECT_EQ(PendingDeleteKeys(), expected_pending);

  auto mismatched = replacement;
  mismatched.index = 1;
  EXPECT_EQ(impl_->CommitChunk(file.ino, expected, mismatched).ToErrno(), EINVAL);

  auto stale_revision = replacement;
  stale_revision.revision = expected.revision;
  EXPECT_EQ(impl_->CommitChunk(file.ino, expected, stale_revision).ToErrno(), EINVAL);
}

FIBER_TEST_F(RedisMetaImplTest, ChunkMutationsRejectInvalidRevision) {
  SwordFsChunk invalid{.index = 0, .revision = swordfs::metadata::kInvalidChunkRevision, .size = 64};
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, invalid).ToErrno(), EINVAL);
}

FIBER_TEST_F(RedisMetaImplTest, SymlinkAndLinkValidateLongNamesAndParentTypes) {
  const std::string long_name(256, 'x');
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, long_name, "target", nullptr).ToErrno() == ENAMETOOLONG);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, long_name, nullptr).ToErrno() == ENAMETOOLONG);
  EXPECT_TRUE(impl_->Symlink(file.ino, "child", "target", nullptr).ToErrno() == ENOTDIR);
  EXPECT_TRUE(impl_->Link(file.ino, file.ino, "child", nullptr).ToErrno() == ENOTDIR);

  SwordFsInode link;
  ASSERT_TRUE(impl_->Symlink(kRootInodeId, "link", "target", &link).ok());
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "link", "target", nullptr).ToErrno() == EEXIST);
}

FIBER_TEST_F(RedisMetaImplTest, UnlinkRejectsLinkCountUnderflowWithoutRemovingEntry) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  file.attr.nlink = 0;
  std::string value;
  ASSERT_TRUE(file.SerializeTo(&value).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(file.ino), value); });

  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").ToErrno() == EIO);
  EXPECT_TRUE(RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { return redis.hexists(key.Directory(kRootInodeId), "file"); }));
}

FIBER_TEST_F(RedisMetaImplTest, RmDirRejectsParentLinkCountUnderflowWithoutRemovingDirectory) {
  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  std::string value = RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { return redis.get(key.Inode(kRootInodeId)).value_or(""); });
  SwordFsInode root;
  ASSERT_TRUE(root.ParseFrom(value).ok());
  root.attr.nlink = 2;
  ASSERT_TRUE(root.SerializeTo(&value).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(kRootInodeId), value); });

  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").ToErrno() == EIO);
  const auto [entry_exists, inode_exists] = RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    return std::pair{redis.hexists(key.Directory(kRootInodeId), "dir"), redis.exists(key.Inode(dir.ino))};
  });
  EXPECT_TRUE(entry_exists);
  EXPECT_TRUE(inode_exists);
}

FIBER_TEST_F(RedisMetaImplTest, MalformedParentMetadataIsRejectedAcrossMutatingOperations) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(kRootInodeId), "malformed"); });

  SwordFsInode out;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "x", &out).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Create(kRootInodeId, "x", 0644, nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "x", 0755, nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "x").ToErrno() == EIO);
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "x").ToErrno() == EIO);
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "x", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
              EIO);
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "x", "target", nullptr).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, MalformedInodeMetadataIsRejectedAcrossReadAndWriteOperations) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(file.ino), "malformed"); });

  SwordFsInode out;
  SwordFsAttr attr;
  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 1};
  std::optional<ReclaimWork> reclaim_work;
  std::string target;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &out).ToErrno() == EIO);
  EXPECT_TRUE(impl_->GetInode(file.ino, &out).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").ToErrno() == EIO);
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      EIO);
  EXPECT_TRUE(impl_->SetAttr(file.ino, attr, SetAttrField::kMode, nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Readlink(file.ino, &target).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Open(file.ino).ToErrno() == EIO);
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &reclaim_work).ToErrno() == EIO);
  EXPECT_FALSE(reclaim_work.has_value());
  EXPECT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Truncate(file.ino, 1).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, MalformedDirectoryEntryIsRejectedByEntryConsumers) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Directory(kRootInodeId), "file", "malformed"); });

  SwordFsInode out;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &out).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Create(kRootInodeId, "file", 0644, nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "file", 0755, nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "file", "target", nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "file", nullptr).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").ToErrno() == EIO);
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "file").ToErrno() == EIO);
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).ToErrno() ==
      EIO);
}

FIBER_TEST_F(RedisMetaImplTest, MalformedChunkMetadataIsRejectedByFindAndTruncate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 4096).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", "malformed"); });

  SwordFsChunk chunk;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &chunk).ToErrno() == EIO);
  EXPECT_TRUE(impl_->Truncate(file.ino, 100).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, TruncateRejectsInvalidPersistedChunkExtent) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "non-canonical-chunk", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 8192).ok());

  SwordFsChunk chunk{.index = 1, .revision = 7, .size = kTestChunkSize + 1};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), std::to_string(chunk.index), encoded); });

  EXPECT_TRUE(impl_->Truncate(file.ino, 0).ToErrno() == EIO);
  EXPECT_TRUE(RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { return redis.hexists(key.Chunk(file.ino), std::to_string(chunk.index)); }));
  EXPECT_FALSE(RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    return redis.hexists(key.PendingDeletes(),
                         swordfs::chunk::FormatChunkObjectKey(file.ino, chunk.index, chunk.revision));
  }));
}

FIBER_TEST_F(RedisMetaImplTest, TruncateCleanupRegistrationFailureDoesNotInvalidateMutation) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "pending-delete-wrongtype", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .revision = 9, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.PendingDeletes(), "wrong-type"); });

  const auto status = impl_->Truncate(file.ino, 0);
  EXPECT_TRUE(status.ok()) << status.message();

  SwordFsChunk stored;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &stored).IsNotFound());
  SwordFsInode after;
  ASSERT_TRUE(impl_->GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.attr.size, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrShrinkCleanupRegistrationFailureDoesNotInvalidateMutation) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "setattr-pending-delete-wrongtype", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .revision = 10, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());

  SwordFsInode before;
  ASSERT_TRUE(impl_->GetInode(file.ino, &before).ok());
  ASSERT_EQ(before.attr.size, 64U);

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.PendingDeletes(), "wrong-type"); });

  SwordFsAttr requested = before.attr;
  requested.size = 0;
  const auto status = impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr);
  EXPECT_TRUE(status.ok()) << status.message();

  SwordFsChunk stored;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &stored).IsNotFound());
  SwordFsInode after;
  ASSERT_TRUE(impl_->GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.attr.size, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, FindChunkRejectsFieldDescriptorIdentityMismatch) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "bad-chunk-identity", 0644, &file).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);

  SwordFsChunk wrong{.index = 1, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", encoded); });

  SwordFsChunk found;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &found).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, FindChunkRejectsInvalidExtentWithMatchingField) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "bad-chunk-layout", 0644, &file).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);

  SwordFsChunk wrong{.index = 0, .revision = 1, .size = kTestChunkSize + 1};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", encoded); });

  SwordFsChunk found;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &found).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, TruncateRejectsWrongTypeChunkMapBeforeMutation) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "wrong-type-chunks", 0644, &file).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.del(key.Chunk(file.ino));
    redis.set(key.Chunk(file.ino), "wrong-type");
  });

  EXPECT_FALSE(impl_->Truncate(file.ino, 0).ok());
  SwordFsInode after;
  ASSERT_TRUE(impl_->GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.attr.size, 64U);
}

FIBER_TEST_F(RedisMetaImplTest, LoadVolumeRejectsCorruptPersistentState) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);

  SwordFsVolume volume;
  volume.name = volume_name_;
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Format(), "malformed"); });
  EXPECT_TRUE(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->LoadVolume(&volume); }).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, StatFsIgnoresAdvisoryInodeCountState) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  const auto limits = impl_->GetLimits();
  auto expect_virtual_capacity = [&] {
    swordfs::metadata::SwordFsStatFs stat;
    ASSERT_TRUE(impl_->StatFs(&stat).ok());
    EXPECT_GT(stat.files, 0U);
    EXPECT_EQ(stat.files, limits.max_free_inodes);
    EXPECT_EQ(stat.files_free, stat.files);
  };

  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.del(key.InodeCount()); });
  expect_virtual_capacity();

  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.InodeCount(), "02"); });
  expect_virtual_capacity();

  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.del(key.InodeCount());
    redis.hset(key.InodeCount(), "wrong", "type");
  });
  expect_virtual_capacity();
}

FIBER_TEST_F(RedisMetaImplTest, MissingMetadataReturnsNotFoundConsistently) {
  constexpr InodeID kMissingIno = 999999;
  SwordFsInode inode;
  SwordFsAttr attr;
  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 1};
  std::string target;
  swordfs::metadata::DirIteratorPtr iterator;

  EXPECT_TRUE(impl_->Lookup(kMissingIno, "x", &inode).IsNotFound());
  EXPECT_TRUE(impl_->GetInode(kMissingIno, &inode).IsNotFound());
  EXPECT_TRUE(impl_->OpenDir(kMissingIno, &iterator).IsNotFound());
  EXPECT_TRUE(impl_->Unlink(kMissingIno, "x").IsNotFound());
  EXPECT_TRUE(impl_->RmDir(kMissingIno, "x").IsNotFound());
  EXPECT_TRUE(impl_->Rename(kMissingIno, "x", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone).IsNotFound());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "missing", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone).IsNotFound());
  EXPECT_TRUE(impl_->SetAttr(kMissingIno, attr, SetAttrField::kMode, nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Symlink(kMissingIno, "x", "target", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Link(kMissingIno, kRootInodeId, "x", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Readlink(kMissingIno, &target).IsNotFound());
  EXPECT_TRUE(impl_->Open(kMissingIno).IsNotFound());
  EXPECT_TRUE(impl_->CommitChunk(kMissingIno, std::nullopt, chunk).IsNotFound());
  EXPECT_TRUE(impl_->Truncate(kMissingIno, 1).IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, CreatePreservesRequestUidAcrossRedisWorker) {
  SwordFsInode parent;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "parent", 0755, &parent).ok());

  SwordFsAttr attr = parent.attr;
  attr.uid = 1234;
  ASSERT_TRUE(impl_->SetAttr(parent.ino, attr, SetAttrField::kUid, &parent).ok());

  SwordFsContext ctx;
  ctx.uid = 1234;
  ctx.gid = 1234;
  folly::fibers::local<SwordFsContext>() = ctx;

  SwordFsInode child;
  ASSERT_TRUE(impl_->MkDir(parent.ino, "child", 0755, &child).ok());
  EXPECT_EQ(child.attr.uid, 1234U);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(child.ino, "file", 0600, &file).ok());
  EXPECT_EQ(file.attr.uid, 1234U);
}

TEST_F(RedisMetaImplTest, LoadVolumeUsesVolumeMetadataAndValidatesName) {
  RedisMetaConfig config = config_;
  const std::string other_name = volume_name_ + "-unformatted";
  RedisMetaImpl other(config, other_name);
  ASSERT_TRUE(other.Initialize().ok());
  SwordFsVolume volume;
  volume.name = other_name;
  EXPECT_TRUE(other.LoadVolume(&volume).IsNotFound());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  WithRawRedisOnThread([&](sw::redis::Redis &redis) { redis.del(key.Inode(kRootInodeId)); });
  volume.name = volume_name_;
  ASSERT_TRUE(impl_->LoadVolume(&volume).ok());
  EXPECT_EQ(volume.name, volume_name_);

  SwordFsVolume malformed = volume;
  malformed.name = volume_name_ + "-mismatch";
  WithRawRedisOnThread([&](sw::redis::Redis &redis) { redis.set(key.Format(), malformed.SerializeTo()); });
  volume.name = volume_name_;
  EXPECT_TRUE(impl_->LoadVolume(&volume).ToErrno() == EIO);
}

// ════════════════════════════════════════════════════════════════════
// Reclaim — durable orphan candidates and frozen pending records
// ════════════════════════════════════════════════════════════════════
//
// Both records are Redis state, so they survive a crash: the orphan
// candidate is what a later mount promotes, and the pending record is what
// lets a later mount finish the deletes without re-deriving any object
// identity from live inode state.
}  // namespace
