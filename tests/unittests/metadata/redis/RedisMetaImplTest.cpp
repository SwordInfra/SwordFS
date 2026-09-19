// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Reclaim.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"

namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
using swordfs::metadata::ReclaimWork;
using swordfs::metadata::RedisMetaConfig;
using swordfs::metadata::RedisMetaImpl;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsVolume;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

bool LoadConfig(RedisMetaConfig *config) {
  const char *url = std::getenv("SWORDFS_REDIS_TEST_URL");
  if (url == nullptr) {
    return false;
  }
  const auto status = swordfs::metadata::ParseRedisMetaUrl(url, config);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok();
}

class RedisMetaImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!LoadConfig(&config_)) {
      GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
    }
    static std::atomic<uint64_t> sequence{0};
    // FormatVolume refuses an already-formatted volume, and Redis state
    // outlives this process. Include the pid so rerunning the test binary
    // against the same Redis instance does not collide with previous runs.
    volume_name_ = "redis-meta-test-" + std::to_string(::getpid()) + "-" + std::to_string(++sequence);
    impl_ = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    auto status = impl_->Initialize();
    ASSERT_TRUE(status.ok()) << status.message();
    SwordFsVolume volume;
    volume.name = volume_name_;
    volume.chunk_size = 4096;
    status = impl_->FormatVolume(volume);
    ASSERT_TRUE(status.ok()) << status.message();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }

  template <typename Fn>
  decltype(auto) WithRawRedisOnThread(Fn &&fn) const {
    swordfs::utils::ExpectInThreadDomain();
    sw::redis::ConnectionOptions options;
    options.host = config_.host;
    options.port = config_.port;
    options.db = config_.db;
    if (config_.username.has_value()) {
      options.user = *config_.username;
    }
    if (config_.password.has_value()) {
      options.password = *config_.password;
    }
    sw::redis::Redis redis(options);
    return std::invoke(std::forward<Fn>(fn), redis);
  }

  template <typename Fn>
  decltype(auto) RunWithRawRedisFromFiber(Fn &&fn) const {
    return swordfs::test::RunInTestThreadFromFiber(
        [this, fn = std::forward<Fn>(fn)]() mutable -> decltype(auto) { return WithRawRedisOnThread(std::move(fn)); });
  }

  std::unique_ptr<RedisMetaImpl> impl_;
  RedisMetaConfig config_;
  std::string volume_name_;
};

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
  EXPECT_EQ(impl_->AllocateChunkRevision(nullptr).code(), swordfs::utils::Status::kInvalidArgument);

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
  ASSERT_TRUE(first_iterator->Peek(&entry, &next_offset).ok());
  EXPECT_EQ(entry.name, ".");
  EXPECT_EQ(next_offset, 1);
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
  chunk0.start_offset = 0;
  chunk0.revision = 1;
  chunk0.size = 4096;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk0).ok());

  SwordFsChunk chunk1;
  chunk1.index = 1;
  chunk1.start_offset = 4096;
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
  EXPECT_EQ(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->LoadVolume(nullptr); }).code(),
            Status::kInvalidArgument);
  EXPECT_TRUE(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->FormatVolume(volume); }).IsAlreadyExists());

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
  EXPECT_EQ(impl_->Lookup(kRootInodeId, "file", nullptr).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->GetInode(file.ino, nullptr).code(), Status::kInvalidArgument);
  EXPECT_TRUE(impl_->Lookup(file.ino, "child", &found).IsNotDirectory());
}

FIBER_TEST_F(RedisMetaImplTest, CreateAndMkdirValidateNamesParentsAndDuplicates) {
  const std::string long_name(256, 'x');
  EXPECT_TRUE(impl_->Create(kRootInodeId, long_name, 0644, nullptr).IsNameTooLong());
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, long_name, 0755, nullptr).IsNameTooLong());

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_TRUE(impl_->Create(kRootInodeId, "file", 0644, nullptr).IsAlreadyExists());
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "file", 0755, nullptr).IsAlreadyExists());
  EXPECT_TRUE(impl_->Create(file.ino, "child", 0644, nullptr).IsNotDirectory());
  EXPECT_TRUE(impl_->MkDir(file.ino, "child", 0755, nullptr).IsNotDirectory());
  EXPECT_TRUE(impl_->Create(999999, "child", 0644, nullptr).IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, UnlinkAndRmdirCoverSuccessAndTypeChecks) {
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());

  EXPECT_EQ(impl_->Unlink(kRootInodeId, ".").code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->RmDir(kRootInodeId, "..").code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->Unlink(kRootInodeId, "dir").code(), Status::kInvalidArgument);
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "file").IsNotDirectory());

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
  EXPECT_TRUE(work->chunks.empty());
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());

  SwordFsInode child;
  ASSERT_TRUE(impl_->Create(dir.ino, "child", 0644, &child).ok());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").IsNotEmpty());
  ASSERT_TRUE(impl_->Unlink(dir.ino, "child").ok());
  ASSERT_TRUE(impl_->RmDir(kRootInodeId, "dir").ok());
}

FIBER_TEST_F(RedisMetaImplTest, RenameCoversMoveOverwriteNoReplaceAndExchange) {
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "second", 0644, &second).ok());

  EXPECT_TRUE(impl_->Rename(kRootInodeId, ".", kRootInodeId, "x", swordfs::metadata::RenameFlag::kNone).IsBusy());
  const std::string long_name(256, 'x');
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "first", kRootInodeId, long_name, swordfs::metadata::RenameFlag::kNone)
                  .IsNameTooLong());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", swordfs::metadata::RenameFlag::kNoReplace)
                  .IsAlreadyExists());
  EXPECT_EQ(impl_
                ->Rename(kRootInodeId, "first", kRootInodeId, "second",
                         swordfs::metadata::RenameFlag::kNoReplace | swordfs::metadata::RenameFlag::kExchange)
                .code(),
            Status::kInvalidArgument);
  EXPECT_EQ(
      impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", static_cast<swordfs::metadata::RenameFlag>(1u << 7))
          .code(),
      Status::kInvalidArgument);

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

FIBER_TEST_F(RedisMetaImplTest, RenameRejectsTypeMismatchAndDirectoryCycles) {
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "dir", swordfs::metadata::RenameFlag::kNone).IsDirectory());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kNone).IsNotDirectory());
  EXPECT_EQ(impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kExchange).code(),
            Status::kInvalidArgument);

  SwordFsInode child;
  ASSERT_TRUE(impl_->MkDir(dir.ino, "child", 0755, &child).ok());
  EXPECT_EQ(impl_->Rename(kRootInodeId, "dir", child.ino, "moved", swordfs::metadata::RenameFlag::kNone).code(),
            Status::kInvalidArgument);
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrAccessAndStatFsCoverCommonFields) {
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

  SwordFsContext ctx;
  ctx.uid = 123;
  ctx.gid = 456;
  folly::fibers::local<SwordFsContext>() = ctx;
  EXPECT_TRUE(impl_->Access(file.ino, R_OK).ok());
  EXPECT_TRUE(impl_->Access(file.ino, W_OK).ok());
  EXPECT_TRUE(impl_->Access(file.ino, X_OK).IsPermission());

  swordfs::metadata::SwordFsStatFs stat;
  ASSERT_TRUE(impl_->StatFs(&stat).ok());
  EXPECT_GE(stat.files, 2U);
  EXPECT_EQ(stat.name_max, impl_->GetLimits().max_name_length);
  EXPECT_EQ(impl_->StatFs(nullptr).code(), Status::kInvalidArgument);
}

FIBER_TEST_F(RedisMetaImplTest, SymlinkHardLinkAndOpenBehaveLikePosixMetadata) {
  SwordFsInode link;
  ASSERT_TRUE(impl_->Symlink(kRootInodeId, "link", "target/path", &link).ok());
  std::string target;
  ASSERT_TRUE(impl_->Readlink(link.ino, &target).ok());
  EXPECT_EQ(target, "target/path");
  EXPECT_EQ(impl_->Readlink(link.ino, nullptr).code(), Status::kInvalidArgument);
  EXPECT_TRUE(impl_->Open(link.ino).IsNotDirectory());

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_EQ(impl_->Readlink(file.ino, &target).code(), Status::kInvalidArgument);
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", &file).ok());
  EXPECT_EQ(file.attr.nlink, 2U);
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).IsAlreadyExists());
  EXPECT_TRUE(impl_->Link(link.ino, kRootInodeId, "hard-link", nullptr).ok());

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  EXPECT_TRUE(impl_->Link(dir.ino, kRootInodeId, "dir-hard", nullptr).IsNotPermitted());
  EXPECT_TRUE(impl_->Open(file.ino).ok());
}

FIBER_TEST_F(RedisMetaImplTest, ChunkVisitFindAndTruncateCoverSparseMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 9000).ok());

  SwordFsChunk chunk0{.index = 0, .start_offset = 0, .revision = 1, .size = 4096};
  SwordFsChunk chunk2{.index = 2, .start_offset = 8192, .revision = 2, .size = 808};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk0).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk2).ok());

  std::vector<swordfs::metadata::ChunkIndex> seen;
  ASSERT_TRUE(impl_
                  ->VisitChunks(file.ino,
                                [&](const SwordFsChunk &chunk) {
                                  seen.push_back(chunk.index);
                                  return Status::OK();
                                })
                  .ok());
  EXPECT_EQ(seen.size(), 2U);
  EXPECT_EQ(impl_->VisitChunks(file.ino, {}).code(), Status::kInvalidArgument);

  SwordFsChunk found;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 2, &found).ok());
  EXPECT_EQ(found.index, 2U);
  EXPECT_EQ(impl_->FindChunk(file.ino, 2, nullptr).code(), Status::kInvalidArgument);

  ASSERT_TRUE(impl_->Truncate(file.ino, 4096).ok());
  EXPECT_TRUE(impl_->FindChunk(file.ino, 2, &found).IsNotFound());
  ASSERT_TRUE(impl_->Truncate(file.ino, 0).ok());
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &found).IsNotFound());
  ASSERT_TRUE(impl_->Truncate(file.ino, 0).ok());
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrShrinkQueuesOnlyMaterializedSparseObjectsForDurableDelete) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "sparse-cleanup", 0644, &file).ok());

  constexpr swordfs::metadata::ChunkIndex kFarIndex = 1000000000U;
  constexpr uint64_t kChunkSize = 4096;
  SwordFsChunk head{.index = 0, .start_offset = 0, .revision = 1, .size = 128};
  SwordFsChunk far{
      .index = kFarIndex, .start_offset = static_cast<uint64_t>(kFarIndex) * kChunkSize, .revision = 2, .size = 128};
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
  std::vector<std::string> pending;
  ASSERT_TRUE(impl_
                  ->VisitPendingDeletes([&pending](const swordfs::metadata::PendingDelete &work) {
                    pending.push_back(work.chunk.key);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending, std::vector<std::string>{far_key});

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
  pending.clear();
  ASSERT_TRUE(peer->VisitPendingDeletes([&pending](const swordfs::metadata::PendingDelete &work) {
                    pending.push_back(work.chunk.key);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending, std::vector<std::string>{far_key});
  ASSERT_TRUE(peer->CompletePendingDelete(far_key).ok());
  pending.clear();
  ASSERT_TRUE(peer->VisitPendingDeletes([&pending](const swordfs::metadata::PendingDelete &work) {
                    pending.push_back(work.chunk.key);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_TRUE(pending.empty());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ChunkAndOpenOperationsRejectWrongInodeTypes) {
  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 1};
  EXPECT_EQ(impl_->CommitChunk(dir.ino, std::nullopt, chunk).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->VisitChunks(dir.ino, [](const SwordFsChunk &) { return Status::OK(); }).code(),
            Status::kInvalidArgument);
  EXPECT_TRUE(impl_->Open(dir.ino).IsNotDirectory());
  EXPECT_TRUE(impl_->OpenDir(dir.ino, nullptr).code() == Status::kInvalidArgument);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  swordfs::metadata::DirIteratorPtr iterator;
  EXPECT_TRUE(impl_->OpenDir(file.ino, &iterator).IsNotDirectory());
}

FIBER_TEST_F(RedisMetaImplTest, ReclaimKeepsLinkedInodesAndRemovesOrphans) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  // A linked inode is never reclaimable: preparation must change nothing.
  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());
  // Both steps are idempotent: repeating them is a no-op, not an error.
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  EXPECT_TRUE(impl_->CompleteReclaim(file.ino).ok());
}

FIBER_TEST_F(RedisMetaImplTest, PermissionChecksRejectMutationsForUnprivilegedCaller) {
  SwordFsAttr root_attr;
  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  root_attr = root.attr;
  root_attr.mode = S_IFDIR | 0555;
  ASSERT_TRUE(impl_->SetAttr(kRootInodeId, root_attr, SetAttrField::kMode, nullptr).ok());

  SwordFsContext ctx;
  ctx.uid = 1000;
  ctx.gid = 1000;
  folly::fibers::local<SwordFsContext>() = ctx;

  EXPECT_TRUE(impl_->Create(kRootInodeId, "create", 0644, nullptr).IsPermission());
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "mkdir", 0755, nullptr).IsPermission());
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "link", "target", nullptr).IsPermission());

  folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());

  folly::fibers::local<SwordFsContext>() = ctx;
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").IsPermission());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").IsPermission());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).IsPermission());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).IsPermission());
}

FIBER_TEST_F(RedisMetaImplTest, StickyDirectoryProtectsEntriesOwnedByOtherUsers) {
  SwordFsInode sticky;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "sticky", 0777, &sticky).ok());
  SwordFsAttr attr = sticky.attr;
  attr.mode = S_IFDIR | 01777;
  ASSERT_TRUE(impl_->SetAttr(sticky.ino, attr, SetAttrField::kMode, &sticky).ok());

  SwordFsContext owner;
  owner.uid = 1001;
  owner.gid = 1001;
  folly::fibers::local<SwordFsContext>() = owner;
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(sticky.ino, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(sticky.ino, "dir", 0755, &dir).ok());

  SwordFsContext other;
  other.uid = 1002;
  other.gid = 1002;
  folly::fibers::local<SwordFsContext>() = other;
  EXPECT_TRUE(impl_->Unlink(sticky.ino, "file").IsPermission());
  EXPECT_TRUE(impl_->RmDir(sticky.ino, "dir").IsPermission());
  EXPECT_TRUE(
      impl_->Rename(sticky.ino, "file", sticky.ino, "other", swordfs::metadata::RenameFlag::kNone).IsPermission());
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

FIBER_TEST_F(RedisMetaImplTest, VisitChunksPropagatesVisitorFailure) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 1};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());

  auto status = impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::IOError("stop"); });
  EXPECT_EQ(status.code(), Status::kIOError);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkInitialPublishIsIdempotentAndGrowsSizeMonotonically) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "publish", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 128U);

  SwordFsChunk later{.index = 2, .start_offset = 8192, .revision = 2, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, later).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 8256U);

  auto conflicting = first;
  conflicting.revision = 3;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, conflicting).IsAlreadyExists());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored.revision, first.revision);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkRewriteUsesCompareAndSwapAndIsIdempotent) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, first).ok());

  SwordFsAttr mode{};
  mode.mode = S_IFREG | 0644 | S_ISUID | S_ISGID;
  ASSERT_TRUE(impl_->SetAttr(file.ino, mode, SetAttrField::kMode, nullptr).ok());

  auto replacement = first;
  replacement.revision = 2;
  replacement.size = 64;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, replacement);

  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 128U);
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), 0U);

  auto stale_replacement = replacement;
  stale_replacement.revision = 3;
  EXPECT_TRUE(impl_->CommitChunk(file.ino, first, stale_replacement).IsAlreadyExists());

  auto grown = replacement;
  grown.revision = 4;
  grown.size = 256;
  ASSERT_TRUE(impl_->CommitChunk(file.ino, replacement, grown).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 256U);
}

FIBER_TEST_F(RedisMetaImplTest, CommitChunkReplayRepairsInodeAfterPartialExec) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace-repair", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
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
  // still re-apply the inode write side effects instead of returning early.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hset(key.Chunk(file.ino), std::to_string(first.index), replacement_value);
  });

  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());
  EXPECT_EQ(file.attr.size, 128U);
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), 0U);

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, replacement);
}

FIBER_TEST_F(RedisMetaImplTest, SizePreservingTruncateReconcilesDescriptorAfterPartialReplace) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace-truncate-repair", 0644, &file).ok());

  SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 5};
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
  SwordFsChunk expected{.index = 0, .start_offset = 0, .revision = 1, .size = 128};
  auto replacement = expected;
  replacement.revision = 2;

  auto invalid_revision = expected;
  invalid_revision.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_EQ(impl_->CommitChunk(999999, invalid_revision, replacement).code(), swordfs::utils::Status::kInvalidArgument);

  auto invalid_replacement = replacement;
  invalid_replacement.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_EQ(impl_->CommitChunk(999999, expected, invalid_replacement).code(), swordfs::utils::Status::kInvalidArgument);

  auto non_canonical_replacement = replacement;
  non_canonical_replacement.start_offset = 1;
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, non_canonical_replacement).code(),
            swordfs::utils::Status::kInvalidArgument);

  auto oversized_replacement = replacement;
  oversized_replacement.size = 4097;
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, oversized_replacement).code(),
            swordfs::utils::Status::kInvalidArgument);

  EXPECT_TRUE(impl_->CommitChunk(999999, expected, replacement).IsNotFound());

  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "replace-invalid-dir", 0755, &dir).ok());
  EXPECT_EQ(impl_->CommitChunk(dir.ino, expected, replacement).code(), swordfs::utils::Status::kInvalidArgument);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "replace-invalid-file", 0644, &file).ok());
  EXPECT_TRUE(impl_->CommitChunk(file.ino, expected, replacement).IsNotFound());

  auto mismatched = replacement;
  mismatched.index = 1;
  EXPECT_EQ(impl_->CommitChunk(file.ino, expected, mismatched).code(), swordfs::utils::Status::kInvalidArgument);

  auto stale_revision = replacement;
  stale_revision.revision = expected.revision;
  EXPECT_EQ(impl_->CommitChunk(file.ino, expected, stale_revision).code(), swordfs::utils::Status::kInvalidArgument);

  mismatched = replacement;
  mismatched.start_offset = 4096;
  EXPECT_EQ(impl_->CommitChunk(file.ino, expected, mismatched).code(), swordfs::utils::Status::kInvalidArgument);

  auto overflowing = replacement;
  overflowing.start_offset = std::numeric_limits<uint64_t>::max() - 16;
  overflowing.size = 32;
  auto overflowing_expected = overflowing;
  overflowing_expected.revision = 3;
  EXPECT_EQ(impl_->CommitChunk(file.ino, overflowing_expected, overflowing).code(),
            swordfs::utils::Status::kInvalidArgument);
}

FIBER_TEST_F(RedisMetaImplTest, ChunkMutationsRejectInvalidRevision) {
  SwordFsChunk invalid{.index = 0, .start_offset = 0, .revision = swordfs::metadata::kInvalidChunkRevision, .size = 64};
  EXPECT_EQ(impl_->CommitChunk(999999, std::nullopt, invalid).code(), swordfs::utils::Status::kInvalidArgument);
}

FIBER_TEST_F(RedisMetaImplTest, SymlinkAndLinkValidateLongNamesAndParentTypes) {
  const std::string long_name(256, 'x');
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, long_name, "target", nullptr).IsNameTooLong());

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, long_name, nullptr).IsNameTooLong());
  EXPECT_TRUE(impl_->Symlink(file.ino, "child", "target", nullptr).IsNotDirectory());
  EXPECT_TRUE(impl_->Link(file.ino, file.ino, "child", nullptr).IsNotDirectory());

  SwordFsInode link;
  ASSERT_TRUE(impl_->Symlink(kRootInodeId, "link", "target", &link).ok());
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "link", "target", nullptr).IsAlreadyExists());
}

FIBER_TEST_F(RedisMetaImplTest, UnlinkRejectsLinkCountUnderflowWithoutRemovingEntry) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  file.attr.nlink = 0;
  std::string value;
  ASSERT_TRUE(file.SerializeTo(&value).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(file.ino), value); });

  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").IsMalformed());
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

  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").IsMalformed());
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
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "x", &out).IsMalformed());
  EXPECT_TRUE(impl_->Create(kRootInodeId, "x", 0644, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "x", 0755, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "x").IsMalformed());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "x").IsMalformed());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "x", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone).IsMalformed());
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "x", "target", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Access(kRootInodeId, R_OK).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedInodeMetadataIsRejectedAcrossReadAndWriteOperations) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(file.ino), "malformed"); });

  SwordFsInode out;
  SwordFsAttr attr;
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 1};
  std::optional<ReclaimWork> reclaim_work;
  std::string target;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &out).IsMalformed());
  EXPECT_TRUE(impl_->GetInode(file.ino, &out).IsMalformed());
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").IsMalformed());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).IsMalformed());
  EXPECT_TRUE(impl_->SetAttr(file.ino, attr, SetAttrField::kMode, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Access(file.ino, R_OK).IsMalformed());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Readlink(file.ino, &target).IsMalformed());
  EXPECT_TRUE(impl_->Open(file.ino).IsMalformed());
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &reclaim_work).IsMalformed());
  EXPECT_FALSE(reclaim_work.has_value());
  EXPECT_TRUE(impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::OK(); }).IsMalformed());
  EXPECT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).IsMalformed());
  EXPECT_TRUE(impl_->Truncate(file.ino, 1).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedDirectoryEntryIsRejectedByEntryConsumers) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Directory(kRootInodeId), "file", "malformed"); });

  SwordFsInode out;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &out).IsMalformed());
  EXPECT_TRUE(impl_->Create(kRootInodeId, "file", 0644, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "file", 0755, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "file", "target", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "file", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file").IsMalformed());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "file").IsMalformed());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedRenameTargetInodeIsRejected) {
  SwordFsInode source;
  SwordFsInode target;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "source", 0644, &source).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "target", 0644, &target).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Inode(target.ino), "malformed"); });

  EXPECT_TRUE(impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kNone)
                  .IsMalformed());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kExchange)
                  .IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedChunkMetadataIsRejectedByVisitorsAndTruncate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 4096).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", "malformed"); });

  SwordFsChunk chunk;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &chunk).IsMalformed());
  EXPECT_TRUE(impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::OK(); }).IsMalformed());
  EXPECT_TRUE(impl_->Truncate(file.ino, 100).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, TruncateRejectsNonCanonicalPersistedChunkIdentity) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "non-canonical-chunk", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 8192).ok());

  SwordFsChunk chunk{.index = 1, .start_offset = 1, .revision = 7, .size = 64};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), std::to_string(chunk.index), encoded); });

  EXPECT_TRUE(impl_->Truncate(file.ino, 0).IsMalformed());
  EXPECT_TRUE(RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { return redis.hexists(key.Chunk(file.ino), std::to_string(chunk.index)); }));
  EXPECT_FALSE(RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    return redis.hexists(key.PendingDeletes(),
                         swordfs::chunk::FormatChunkObjectKey(file.ino, chunk.index, chunk.revision));
  }));
}

FIBER_TEST_F(RedisMetaImplTest, TruncatePreflightsPendingDeleteSchemaBeforeRemovingChunkMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "pending-delete-wrongtype", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 9, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.PendingDeletes(), "wrong-type"); });

  const auto status = impl_->Truncate(file.ino, 0);
  EXPECT_FALSE(status.ok());

  // The preflight read fails before any writes are queued, so the authoritative
  // chunk descriptor and inode size remain intact rather than creating an
  // untracked object leak through a partial EXEC.
  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, chunk);
  SwordFsInode after;
  ASSERT_TRUE(impl_->GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.attr.size, 64U);
}

FIBER_TEST_F(RedisMetaImplTest, SetAttrShrinkStopsWhenPendingDeletePreparationFails) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "setattr-pending-delete-wrongtype", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 10, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());

  SwordFsInode before;
  ASSERT_TRUE(impl_->GetInode(file.ino, &before).ok());
  ASSERT_EQ(before.attr.size, 64U);

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.PendingDeletes(), "wrong-type"); });

  SwordFsAttr requested = before.attr;
  requested.size = 0;
  const auto status = impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr);
  EXPECT_FALSE(status.ok());

  SwordFsChunk stored;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &stored).ok());
  EXPECT_EQ(stored, chunk);
  SwordFsInode after;
  ASSERT_TRUE(impl_->GetInode(file.ino, &after).ok());
  EXPECT_EQ(after.attr.size, before.attr.size);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteScanRejectsFieldValueIdentityMismatch) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  swordfs::metadata::PendingDelete pending;
  pending.ino = 42;
  pending.chunk.descriptor = SwordFsChunk{.index = 0, .start_offset = 0, .revision = 9, .size = 64};
  pending.chunk.key = swordfs::chunk::FormatChunkObjectKey(pending.ino, pending.chunk.descriptor.index,
                                                           pending.chunk.descriptor.revision);
  std::string encoded;
  ASSERT_TRUE(pending.SerializeTo(&encoded).ok());

  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "different/object/key", encoded); });

  EXPECT_TRUE(
      impl_->VisitPendingDeletes([](const swordfs::metadata::PendingDelete &) { return Status::OK(); }).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteScanRejectsMalformedAndNonCanonicalRecords) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "broken", "not-a-record"); });
  EXPECT_TRUE(
      impl_->VisitPendingDeletes([](const swordfs::metadata::PendingDelete &) { return Status::OK(); }).IsMalformed());

  swordfs::metadata::PendingDelete pending;
  pending.ino = 42;
  pending.chunk.descriptor = SwordFsChunk{.index = 1, .start_offset = 1, .revision = 9, .size = 64};
  pending.chunk.key = swordfs::chunk::FormatChunkObjectKey(pending.ino, pending.chunk.descriptor.index,
                                                           pending.chunk.descriptor.revision);
  std::string encoded;
  ASSERT_TRUE(pending.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.del(key.PendingDeletes());
    redis.hset(key.PendingDeletes(), pending.chunk.key, encoded);
  });
  EXPECT_TRUE(
      impl_->VisitPendingDeletes([](const swordfs::metadata::PendingDelete &) { return Status::OK(); }).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, ChunkReadersRejectFieldDescriptorIdentityMismatch) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "bad-chunk-identity", 0644, &file).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);

  SwordFsChunk wrong{.index = 1, .start_offset = 4096, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", encoded); });

  SwordFsChunk found;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &found).IsMalformed());
  EXPECT_TRUE(impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::OK(); }).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, ChunkReadersRejectNonCanonicalDescriptorWithMatchingField) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "bad-chunk-layout", 0644, &file).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);

  SwordFsChunk wrong{.index = 0, .start_offset = 1, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", encoded); });

  SwordFsChunk found;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &found).IsMalformed());
  EXPECT_TRUE(impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::OK(); }).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, TruncateRejectsWrongTypeChunkMapBeforeMutation) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "wrong-type-chunks", 0644, &file).ok());
  ASSERT_TRUE(
      impl_->CommitChunk(file.ino, std::nullopt, SwordFsChunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64})
          .ok());

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

FIBER_TEST_F(RedisMetaImplTest, LoadVolumeAndStatFsRejectCorruptPersistentState) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);

  SwordFsVolume volume;
  volume.name = volume_name_;
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.Format(), "malformed"); });
  EXPECT_TRUE(swordfs::test::RunInTestThreadFromFiber([&] { return impl_->LoadVolume(&volume); }).IsMalformed());

  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.set(key.InodeCount(), "not-a-number"); });
  swordfs::metadata::SwordFsStatFs stat;
  EXPECT_EQ(impl_->StatFs(&stat).code(), Status::kIOError);
}

FIBER_TEST_F(RedisMetaImplTest, MissingMetadataReturnsNotFoundConsistently) {
  constexpr InodeID kMissingIno = 999999;
  SwordFsInode inode;
  SwordFsAttr attr;
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 1};
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
  EXPECT_TRUE(impl_->Access(kMissingIno, R_OK).IsNotFound());
  EXPECT_TRUE(impl_->Symlink(kMissingIno, "x", "target", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Link(kMissingIno, kRootInodeId, "x", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Readlink(kMissingIno, &target).IsNotFound());
  EXPECT_TRUE(impl_->Open(kMissingIno).IsNotFound());
  EXPECT_TRUE(impl_->VisitChunks(kMissingIno, [](const SwordFsChunk &) { return Status::OK(); }).IsNotFound());
  EXPECT_TRUE(impl_->CommitChunk(kMissingIno, std::nullopt, chunk).IsNotFound());
  EXPECT_TRUE(impl_->Truncate(kMissingIno, 1).IsNotFound());
}

FIBER_TEST_F(RedisMetaImplTest, RenameChecksNewParentAndNonEmptyTargetDirectory) {
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
          .IsNotEmpty());

  SwordFsAttr attr = target_parent.attr;
  attr.mode = S_IFDIR | 0555;
  ASSERT_TRUE(impl_->SetAttr(target_parent.ino, attr, SetAttrField::kMode, nullptr).ok());
  SwordFsContext ctx;
  ctx.uid = 2000;
  ctx.gid = 2000;
  folly::fibers::local<SwordFsContext>() = ctx;
  EXPECT_TRUE(
      impl_->Rename(source_parent.ino, "source", target_parent.ino, "moved", swordfs::metadata::RenameFlag::kNone)
          .IsPermission());
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
  EXPECT_TRUE(impl_->Access(file.ino, R_OK | W_OK).ok());
}

FIBER_TEST_F(RedisMetaImplTest, OpenRejectsUnreadableRegularFile) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0600, &file).ok());
  SwordFsContext ctx;
  ctx.uid = 1234;
  ctx.gid = 1234;
  folly::fibers::local<SwordFsContext>() = ctx;
  EXPECT_TRUE(impl_->Open(file.ino).IsPermission());
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
  EXPECT_TRUE(impl_->LoadVolume(&volume).IsMalformed());
}

// ════════════════════════════════════════════════════════════════════
// Reclaim — durable orphan candidates and frozen pending records
// ════════════════════════════════════════════════════════════════════
//
// Both records are Redis state, so they survive a crash: the orphan
// candidate is what a later mount promotes, and the pending record is what
// lets a later mount finish the deletes without re-deriving any object
// identity from live inode state.

FIBER_TEST_F(RedisMetaImplTest, UnlinkPublishesDurableOrphanCandidate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(
      impl_->CommitChunk(file.ino, std::nullopt, SwordFsChunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64})
          .ok());

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::vector<InodeID> orphans;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&orphans](InodeID ino) {
                    orphans.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(orphans, std::vector<InodeID>{file.ino});

  // Durable, and nothing irreversible yet: the inode and its chunk metadata
  // stay until preparation freezes them.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
    EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
  });
  SwordFsInode stored;
  ASSERT_TRUE(impl_->GetInode(file.ino, &stored).ok());
  EXPECT_EQ(stored.attr.nlink, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimFreezesRecordAndRemovesLiveMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  ASSERT_EQ(work->chunks.size(), 1U);
  EXPECT_EQ(work->chunks[0].descriptor, chunk);
  EXPECT_EQ(work->chunks[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 1));

  // The live metadata is gone and only the frozen record remains: the point
  // of no return has been crossed.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
    EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
    EXPECT_FALSE(redis.exists(key.Chunk(file.ino)));
    EXPECT_FALSE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
  });
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "revived", nullptr).IsNotFound());

  // Replaying preparation returns the same frozen work without changing it:
  // that is exactly the crash-recovery path a later mount takes.
  std::optional<ReclaimWork> replay;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &replay).ok());
  EXPECT_EQ(replay, work);

  std::vector<InodeID> pending;
  ASSERT_TRUE(impl_
                  ->VisitPendingReclaims([&pending](const ReclaimWork &pending_work) {
                    pending.push_back(pending_work.ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending, std::vector<InodeID>{file.ino});

  // Completion drops the record, and only then: it is idempotent.
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });
  EXPECT_TRUE(impl_->CompleteReclaim(file.ino).ok());
}

FIBER_TEST_F(RedisMetaImplTest, CrashLeftPendingReclaimIsRetriedFromPersistedState) {
  // Model a crash between "prepared" and "completed": the frozen record is in
  // Redis and the inode is gone. Recovery must hand back the same frozen
  // identities — nothing may be re-derived from live state (there is none).
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 3, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::optional<ReclaimWork> prepared;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &prepared).ok());
  ASSERT_TRUE(prepared.has_value());
  // (crash here — the deletes never run)

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });

  std::vector<InodeID> pending;
  ASSERT_TRUE(impl_
                  ->VisitPendingReclaims([&pending](const ReclaimWork &pending_work) {
                    pending.push_back(pending_work.ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending, std::vector<InodeID>{file.ino});

  std::optional<ReclaimWork> retried;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &retried).ok());
  EXPECT_EQ(retried, prepared);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(retried->chunks[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 3));

  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  ASSERT_TRUE(impl_->VisitPendingReclaims([](const ReclaimWork &) { return Status::OK(); }).ok());
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimConvergesAfterPartialExec) {
  // Force the final INCRBY in DeleteInode to fail during EXEC. Redis does not
  // roll back the earlier successful commands in the transaction, so this
  // deterministically models the partial-EXEC state that reclaim retries must
  // converge from: the frozen record is durable while the live inode/chunk
  // metadata has already been removed.
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 7, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  uint64_t original_inode_count = 0;
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    const auto count = redis.get(key.InodeCount());
    ASSERT_TRUE(count.has_value());
    original_inode_count = std::stoull(*count);
    ASSERT_GT(original_inode_count, 0U);
    redis.set(key.InodeCount(), "not-an-integer");
  });

  std::optional<ReclaimWork> first;
  const auto first_status = impl_->PrepareReclaim(file.ino, &first);
  EXPECT_EQ(first_status.code(), Status::kIOError);
  EXPECT_NE(first_status.message().find("commit may be partial"), std::string::npos);

  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    // HSET(reclaim), HDEL(orphan), DEL(chunk), DEL(inode) executed before the
    // failing INCRBY. The pending record therefore remains the recovery
    // authority even though the caller observed an error.
    EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
    EXPECT_FALSE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
    EXPECT_FALSE(redis.exists(key.Chunk(file.ino)));

    // Repair the intentionally corrupted counter to the value the failed
    // transaction was trying to establish, then retry the reclaim itself.
    redis.set(key.InodeCount(), std::to_string(original_inode_count - 1));
  });

  std::optional<ReclaimWork> retried;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &retried).ok());
  ASSERT_TRUE(retried.has_value());
  ASSERT_EQ(retried->chunks.size(), 1U);
  EXPECT_EQ(retried->chunks[0].descriptor, chunk);
  EXPECT_EQ(retried->chunks[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 7));

  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });
}

FIBER_TEST_F(RedisMetaImplTest, LinkRevivesOrphanCandidateAndClearsMarker) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino))); });

  // The revival drops the marker in the same transaction that re-links the
  // inode, so reconciliation can never reclaim a linked inode.
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "revived", nullptr).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_FALSE(redis.hexists(key.Orphans(), std::to_string(file.ino))); });

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  SwordFsInode stored;
  ASSERT_TRUE(impl_->GetInode(file.ino, &stored).ok());
  EXPECT_EQ(stored.attr.nlink, 1U);
}

FIBER_TEST_F(RedisMetaImplTest, MalformedPendingReclaimRecordIsRejected) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), "4242", "malformed"); });
  std::vector<InodeID> pending;
  EXPECT_TRUE(impl_
                  ->VisitPendingReclaims([&pending](const ReclaimWork &work) {
                    pending.push_back(work.ino);
                    return Status::OK();
                  })
                  .IsMalformed());

  // PrepareReclaim replays an existing frozen record before consulting live
  // inode state. A corrupt pending record must therefore fail closed here too
  // rather than being treated as a fresh reclaim.
  std::optional<ReclaimWork> replay;
  EXPECT_TRUE(impl_->PrepareReclaim(4242, &replay).IsMalformed());
  EXPECT_FALSE(replay.has_value());

  // A decodable record whose serialized inode disagrees with its hash field
  // is corrupt too: acting on it could delete another inode's objects.
  ReclaimWork record;
  record.ino = 7777;
  std::string serialized;
  ASSERT_TRUE(record.SerializeTo(&serialized).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), "4242", serialized); });
  EXPECT_TRUE(impl_->VisitPendingReclaims([](const ReclaimWork &) { return Status::OK(); }).IsMalformed());

  // A key that is not an inode id at all is not a candidate either.
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Reclaims(), "4242");
    redis.hset(key.Orphans(), "not-an-inode", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).IsMalformed());

  // Exercise the other field-parsing failures independently: a numeric
  // prefix with trailing junk, and the reserved zero inode id.
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Orphans(), "not-an-inode");
    redis.hset(key.Orphans(), "42x", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).IsMalformed());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Orphans(), "42x");
    redis.hset(key.Orphans(), "0", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).IsMalformed());
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimRejectsPendingRecordForAnotherInode) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  ReclaimWork wrong;
  wrong.ino = file.ino + 1;
  std::string serialized;
  ASSERT_TRUE(wrong.SerializeTo(&serialized).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), std::to_string(file.ino), serialized); });

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).IsMalformed());
  EXPECT_FALSE(work.has_value());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedChunkMetadataIsRejectedByPrepareReclaim) {
  // A corrupt chunk record must stop the freeze before it writes anything:
  // no pending record may exist and the live inode must survive.
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", "malformed"); });
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  ReclaimWork stale_output;
  stale_output.ino = 9999;
  std::optional<ReclaimWork> work = stale_output;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).IsMalformed());
  EXPECT_FALSE(work.has_value()) << "failed preparation must not leave stale caller-visible work";
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
  });
  SwordFsInode stored;
  EXPECT_TRUE(impl_->GetInode(file.ino, &stored).ok());
}

FIBER_TEST_F(RedisMetaImplTest, VisitorArgumentsAreValidated) {
  // The visitors are how reconciliation reads the durable state: a null
  // visitor is a caller bug and must be refused before any Redis round-trip,
  // never read as "nothing to reclaim".
  EXPECT_EQ(impl_->VisitOrphanCandidates(swordfs::metadata::InodeVisitorFn{}).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->VisitPendingReclaims(swordfs::metadata::ReclaimVisitorFn{}).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->VisitPendingDeletes(swordfs::metadata::PendingDeleteVisitorFn{}).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->PrepareReclaim(kRootInodeId, nullptr).code(), Status::kInvalidArgument);

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(kRootInodeId, &work).ok());
  EXPECT_FALSE(work.has_value());
}

FIBER_TEST_F(RedisMetaImplTest, ReclaimVisitorsReturnSnapshotsAndPropagateAbort) {
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "second", 0644, &second).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "first").ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "second").ok());

  std::vector<InodeID> visited;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&](InodeID ino) {
                    visited.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(visited, (std::vector<InodeID>{first.ino, second.ino}));

  visited.clear();
  const auto orphan_abort = impl_->VisitOrphanCandidates([&](InodeID ino) {
    visited.push_back(ino);
    return Status::Busy("stop orphan scan");
  });
  EXPECT_EQ(orphan_abort.code(), Status::kBusy);
  EXPECT_EQ(visited, (std::vector<InodeID>{first.ino}));

  std::optional<ReclaimWork> frozen;
  ASSERT_TRUE(impl_->PrepareReclaim(second.ino, &frozen).ok());
  ASSERT_TRUE(frozen.has_value());

  visited.clear();
  ASSERT_TRUE(impl_
                  ->VisitPendingReclaims([&](const ReclaimWork &work) {
                    visited.push_back(work.ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(visited, (std::vector<InodeID>{second.ino}));

  visited.clear();
  const auto pending_abort = impl_->VisitPendingReclaims([&](const ReclaimWork &work) {
    visited.push_back(work.ino);
    return Status::IOError("stop pending scan");
  });
  EXPECT_EQ(pending_abort.code(), Status::kIOError);
  EXPECT_EQ(visited, (std::vector<InodeID>{second.ino}));
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteVisitorPropagatesAbort) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "pending-delete-abort", 0644, &file).ok());
  ASSERT_TRUE(
      impl_->CommitChunk(file.ino, std::nullopt, SwordFsChunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64})
          .ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 0).ok());

  size_t visits = 0;
  const auto status = impl_->VisitPendingDeletes([&](const swordfs::metadata::PendingDelete &) {
    ++visits;
    return Status::Busy("stop pending delete scan");
  });
  EXPECT_EQ(status.code(), Status::kBusy);
  EXPECT_EQ(status.message(), "stop pending delete scan");
  EXPECT_EQ(visits, 1U);
}

}  // namespace
