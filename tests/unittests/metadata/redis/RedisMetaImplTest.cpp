// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"

namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
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
    ASSERT_TRUE(impl_->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    volume.meta_url = "redis://test";
    volume.chunk_size = 4096;
    ASSERT_TRUE(impl_->FormatVolume(volume).ok());
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }

  sw::redis::Redis RawRedis() const {
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
    return sw::redis::Redis(options);
  }

  std::unique_ptr<RedisMetaImpl> impl_;
  RedisMetaConfig config_;
  std::string volume_name_;
};

TEST_F(RedisMetaImplTest, OpenDirReturnsIndependentIteratorsAndSupportsSeek) {
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

TEST_F(RedisMetaImplTest, RenameDirectoryOverEmptyDirectoryUpdatesSameParentNlink) {
  SwordFsInode src;
  SwordFsInode dst;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "src", 0777, &src).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dst", 0777, &dst).ok());

  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  ASSERT_EQ(root.attr.nlink, 4U);

  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "src", kRootInodeId, "dst", swordfs::metadata::RenameFlag::kNone, nullptr).ok());

  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  EXPECT_EQ(root.attr.nlink, 3U);
}

TEST_F(RedisMetaImplTest, RenameDirectoryOverEmptyDirectoryUpdatesCrossParentNlink) {
  SwordFsInode dir_a;
  SwordFsInode dir_b;
  SwordFsInode src;
  SwordFsInode dst;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "a", 0777, &dir_a).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "b", 0777, &dir_b).ok());
  ASSERT_TRUE(impl_->MkDir(dir_a.ino, "src", 0777, &src).ok());
  ASSERT_TRUE(impl_->MkDir(dir_b.ino, "dst", 0777, &dst).ok());

  ASSERT_TRUE(impl_->Rename(dir_a.ino, "src", dir_b.ino, "dst", swordfs::metadata::RenameFlag::kNone, nullptr).ok());

  SwordFsInode old_parent;
  SwordFsInode new_parent;
  ASSERT_TRUE(impl_->GetInode(dir_a.ino, &old_parent).ok());
  ASSERT_TRUE(impl_->GetInode(dir_b.ino, &new_parent).ok());
  // The old parent loses the moved directory's ".." backlink; the new parent
  // loses the victim's backlink and gains the moved directory's, net zero.
  EXPECT_EQ(old_parent.attr.nlink, 2U);
  EXPECT_EQ(new_parent.attr.nlink, 3U);
}

TEST_F(RedisMetaImplTest, ConcurrentOpenDoesNotFailOnAtimeContention) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  constexpr size_t kThreadCount = 8;
  const InodeID file_ino = file.ino;
  std::vector<std::thread> threads;
  std::vector<Status> statuses(kThreadCount);
  threads.reserve(kThreadCount);
  for (size_t i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([this, &statuses, file_ino, i] {
      folly::fibers::local<SwordFsContext>() = SwordFsContext{};
      statuses[i] = impl_->Open(file_ino);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (const auto &status : statuses) {
    EXPECT_TRUE(status.ok()) << status.message();
  }
}

TEST_F(RedisMetaImplTest, SetAttrPreservesExplicitCtime) {
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

TEST_F(RedisMetaImplTest, SetAttrSameOwnerKeepsSuidSgid) {
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

TEST_F(RedisMetaImplTest, SetAttrShrinkRemovesAndClampsChunks) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  SwordFsAttr initial = file.attr;
  initial.size = 8192;
  ASSERT_TRUE(impl_->SetAttr(file.ino, initial, SetAttrField::kSize, &file).ok());

  SwordFsChunk chunk0;
  chunk0.index = 0;
  chunk0.start_offset = 0;
  chunk0.size = 4096;
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk0).ok());

  SwordFsChunk chunk1;
  chunk1.index = 1;
  chunk1.start_offset = 4096;
  chunk1.size = 4096;
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk1).ok());

  SwordFsAttr requested = file.attr;
  requested.size = 100;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr).ok());

  SwordFsChunk actual;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &actual).ok());
  EXPECT_EQ(actual.size, 100U);
  EXPECT_TRUE(impl_->FindChunk(file.ino, 1, &actual).IsNotFound());
}

TEST_F(RedisMetaImplTest, VolumeAndBasicLookupOperations) {
  SwordFsVolume volume;
  ASSERT_TRUE(impl_->LoadVolume(&volume).ok());
  EXPECT_EQ(volume.name, volume_name_);
  EXPECT_EQ(volume.chunk_size, 4096U);
  EXPECT_EQ(impl_->LoadVolume(nullptr).code(), Status::kInvalidArgument);
  EXPECT_TRUE(impl_->FormatVolume(volume).IsAlreadyExists());

  const auto limits = impl_->GetLimits();
  EXPECT_EQ(limits.max_name_length, 255U);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "file", &found).ok());
  EXPECT_EQ(found.ino, file.ino);
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "missing", &found).IsNotFound());
  EXPECT_EQ(impl_->Lookup(kRootInodeId, "file", nullptr).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->GetInode(file.ino, nullptr).code(), Status::kInvalidArgument);
  EXPECT_TRUE(impl_->Lookup(file.ino, "child", &found).IsNotDirectory());
}

TEST_F(RedisMetaImplTest, CreateAndMkdirValidateNamesParentsAndDuplicates) {
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

TEST_F(RedisMetaImplTest, UnlinkAndRmdirCoverSuccessAndTypeChecks) {
  SwordFsInode file;
  SwordFsInode dir;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());

  EXPECT_EQ(impl_->Unlink(kRootInodeId, ".", nullptr).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->RmDir(kRootInodeId, "..").code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->Unlink(kRootInodeId, "dir", nullptr).code(), Status::kInvalidArgument);
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "file").IsNotDirectory());

  uint64_t post_nlink = 99;
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file", &post_nlink).ok());
  EXPECT_EQ(post_nlink, 0U);
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &file).IsNotFound());
  ASSERT_TRUE(impl_->ReclaimInode(file.ino).ok());
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());

  SwordFsInode child;
  ASSERT_TRUE(impl_->Create(dir.ino, "child", 0644, &child).ok());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").IsNotEmpty());
  ASSERT_TRUE(impl_->Unlink(dir.ino, "child", nullptr).ok());
  ASSERT_TRUE(impl_->RmDir(kRootInodeId, "dir").ok());
}

TEST_F(RedisMetaImplTest, RenameCoversMoveOverwriteNoReplaceAndExchange) {
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "second", 0644, &second).ok());

  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, ".", kRootInodeId, "x", swordfs::metadata::RenameFlag::kNone, nullptr).IsBusy());
  const std::string long_name(256, 'x');
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "first", kRootInodeId, long_name, swordfs::metadata::RenameFlag::kNone, nullptr)
          .IsNameTooLong());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", swordfs::metadata::RenameFlag::kNoReplace, nullptr)
          .IsAlreadyExists());

  swordfs::metadata::RenameResult result;
  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "first", kRootInodeId, "second", swordfs::metadata::RenameFlag::kNone, &result).ok());
  EXPECT_EQ(result.overwritten_ino, second.ino);
  EXPECT_EQ(result.overwritten_post_nlink, 0U);

  SwordFsInode third;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "third", 0644, &third).ok());
  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "second", kRootInodeId, "third", swordfs::metadata::RenameFlag::kExchange, nullptr)
          .ok());
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "second", &found).ok());
  EXPECT_EQ(found.ino, third.ino);
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "third", &found).ok());
  EXPECT_EQ(found.ino, first.ino);

  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "third", kRootInodeId, "missing", swordfs::metadata::RenameFlag::kExchange, nullptr)
          .IsNotFound());
}

TEST_F(RedisMetaImplTest, RenameRejectsTypeMismatchAndDirectoryCycles) {
  SwordFsInode dir;
  SwordFsInode file;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  EXPECT_TRUE(impl_->Rename(kRootInodeId, "file", kRootInodeId, "dir", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsDirectory());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsNotDirectory());
  EXPECT_EQ(impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kExchange, nullptr)
                .code(),
            Status::kInvalidArgument);

  SwordFsInode child;
  ASSERT_TRUE(impl_->MkDir(dir.ino, "child", 0755, &child).ok());
  EXPECT_EQ(
      impl_->Rename(kRootInodeId, "dir", child.ino, "moved", swordfs::metadata::RenameFlag::kNone, nullptr).code(),
      Status::kInvalidArgument);
}

TEST_F(RedisMetaImplTest, SetAttrAccessAndStatFsCoverCommonFields) {
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
  EXPECT_EQ(stat.name_max, 255U);
  EXPECT_EQ(impl_->StatFs(nullptr).code(), Status::kInvalidArgument);
}

TEST_F(RedisMetaImplTest, SymlinkHardLinkAndOpenBehaveLikePosixMetadata) {
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

TEST_F(RedisMetaImplTest, ChunkVisitFindAndTruncateCoverSparseMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 9000).ok());

  SwordFsChunk chunk0{.index = 0, .start_offset = 0, .size = 4096};
  SwordFsChunk chunk2{.index = 2, .start_offset = 8192, .size = 808};
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk0).ok());
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk2).ok());

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

TEST_F(RedisMetaImplTest, ChunkAndOpenOperationsRejectWrongInodeTypes) {
  SwordFsInode dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dir", 0755, &dir).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .size = 1};
  EXPECT_EQ(impl_->AddChunk(dir.ino, chunk).code(), Status::kInvalidArgument);
  EXPECT_EQ(impl_->VisitChunks(dir.ino, [](const SwordFsChunk &) { return Status::OK(); }).code(),
            Status::kInvalidArgument);
  EXPECT_TRUE(impl_->Open(dir.ino).IsNotDirectory());
  EXPECT_TRUE(impl_->OpenDir(dir.ino, nullptr).code() == Status::kInvalidArgument);

  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  swordfs::metadata::DirIteratorPtr iterator;
  EXPECT_TRUE(impl_->OpenDir(file.ino, &iterator).IsNotDirectory());
}

TEST_F(RedisMetaImplTest, ReclaimKeepsLinkedInodesAndRemovesOrphans) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->ReclaimInode(file.ino).ok());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file", nullptr).ok());
  ASSERT_TRUE(impl_->ReclaimInode(file.ino).ok());
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());
  EXPECT_TRUE(impl_->ReclaimInode(file.ino).ok());
}

TEST_F(RedisMetaImplTest, PermissionChecksRejectMutationsForUnprivilegedCaller) {
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
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file", nullptr).IsPermission());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "dir").IsPermission());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsPermission());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).IsPermission());
}

TEST_F(RedisMetaImplTest, StickyDirectoryProtectsEntriesOwnedByOtherUsers) {
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
  EXPECT_TRUE(impl_->Unlink(sticky.ino, "file", nullptr).IsPermission());
  EXPECT_TRUE(impl_->RmDir(sticky.ino, "dir").IsPermission());
  EXPECT_TRUE(impl_->Rename(sticky.ino, "file", sticky.ino, "other", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsPermission());
}

TEST_F(RedisMetaImplTest, RenameExchangeDirectoriesAcrossParentsUpdatesParents) {
  SwordFsInode left;
  SwordFsInode right;
  SwordFsInode left_dir;
  SwordFsInode right_dir;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "left", 0755, &left).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "right", 0755, &right).ok());
  ASSERT_TRUE(impl_->MkDir(left.ino, "a", 0755, &left_dir).ok());
  ASSERT_TRUE(impl_->MkDir(right.ino, "b", 0755, &right_dir).ok());

  ASSERT_TRUE(impl_->Rename(left.ino, "a", right.ino, "b", swordfs::metadata::RenameFlag::kExchange, nullptr).ok());

  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(left.ino, "a", &found).ok());
  EXPECT_EQ(found.ino, right_dir.ino);
  EXPECT_EQ(found.parent_ino, left.ino);
  ASSERT_TRUE(impl_->Lookup(right.ino, "b", &found).ok());
  EXPECT_EQ(found.ino, left_dir.ino);
  EXPECT_EQ(found.parent_ino, right.ino);
}

TEST_F(RedisMetaImplTest, RenameSameInodeThroughHardLinkIsNoOp) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "alias", nullptr).ok());

  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "alias", swordfs::metadata::RenameFlag::kNone, nullptr).ok());
  ASSERT_TRUE(
      impl_->Rename(kRootInodeId, "file", kRootInodeId, "alias", swordfs::metadata::RenameFlag::kExchange, nullptr)
          .ok());
}

TEST_F(RedisMetaImplTest, SetAttrNowAndGrowPreserveExpectedMetadata) {
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

TEST_F(RedisMetaImplTest, VisitChunksPropagatesVisitorFailure) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .size = 1};
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk).ok());

  auto status = impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::IOError("stop"); });
  EXPECT_EQ(status.code(), Status::kIOError);
}

TEST_F(RedisMetaImplTest, SymlinkAndLinkValidateLongNamesAndParentTypes) {
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

TEST_F(RedisMetaImplTest, MalformedParentMetadataIsRejectedAcrossMutatingOperations) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();
  redis.set(key.Inode(kRootInodeId), "malformed");

  SwordFsInode out;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "x", &out).IsMalformed());
  EXPECT_TRUE(impl_->Create(kRootInodeId, "x", 0644, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->MkDir(kRootInodeId, "x", 0755, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "x", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "x").IsMalformed());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "x", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Symlink(kRootInodeId, "x", "target", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Access(kRootInodeId, R_OK).IsMalformed());
}

TEST_F(RedisMetaImplTest, MalformedInodeMetadataIsRejectedAcrossReadAndWriteOperations) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();
  redis.set(key.Inode(file.ino), "malformed");

  SwordFsInode out;
  SwordFsAttr attr;
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .size = 1};
  std::string target;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &out).IsMalformed());
  EXPECT_TRUE(impl_->GetInode(file.ino, &out).IsMalformed());
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsMalformed());
  EXPECT_TRUE(impl_->SetAttr(file.ino, attr, SetAttrField::kMode, nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Access(file.ino, R_OK).IsMalformed());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "hard", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->Readlink(file.ino, &target).IsMalformed());
  EXPECT_TRUE(impl_->Open(file.ino).IsMalformed());
  EXPECT_TRUE(impl_->ReclaimInode(file.ino).IsMalformed());
  EXPECT_TRUE(impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::OK(); }).IsMalformed());
  EXPECT_TRUE(impl_->AddChunk(file.ino, chunk).IsMalformed());
  EXPECT_TRUE(impl_->Truncate(file.ino, 1).IsMalformed());
}

TEST_F(RedisMetaImplTest, MalformedDirectoryEntryIsRejectedByEntryConsumers) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();
  redis.hset(key.Directory(kRootInodeId), "file", "malformed");

  SwordFsInode out;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "file", &out).IsMalformed());
  EXPECT_TRUE(impl_->Unlink(kRootInodeId, "file", nullptr).IsMalformed());
  EXPECT_TRUE(impl_->RmDir(kRootInodeId, "file").IsMalformed());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "file", kRootInodeId, "moved", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsMalformed());
}

TEST_F(RedisMetaImplTest, MalformedRenameTargetInodeIsRejected) {
  SwordFsInode source;
  SwordFsInode target;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "source", 0644, &source).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "target", 0644, &target).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();
  redis.set(key.Inode(target.ino), "malformed");

  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kNone, nullptr)
          .IsMalformed());
  EXPECT_TRUE(
      impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kExchange, nullptr)
          .IsMalformed());
}

TEST_F(RedisMetaImplTest, MalformedChunkMetadataIsRejectedByVisitorsAndTruncate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Truncate(file.ino, 4096).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();
  redis.hset(key.Chunk(file.ino), "0", "malformed");

  SwordFsChunk chunk;
  EXPECT_TRUE(impl_->FindChunk(file.ino, 0, &chunk).IsMalformed());
  EXPECT_TRUE(impl_->VisitChunks(file.ino, [](const SwordFsChunk &) { return Status::OK(); }).IsMalformed());
  EXPECT_TRUE(impl_->Truncate(file.ino, 100).IsMalformed());
}

TEST_F(RedisMetaImplTest, LoadVolumeAndStatFsRejectCorruptPersistentState) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();

  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  std::string root_value;
  ASSERT_TRUE(root.SerializeTo(&root_value).ok());

  SwordFsVolume volume;
  redis.set(key.Inode(kRootInodeId), "malformed");
  EXPECT_TRUE(impl_->LoadVolume(&volume).IsMalformed());
  redis.set(key.Inode(kRootInodeId), root_value);

  redis.set(key.Format(), "malformed");
  EXPECT_TRUE(impl_->LoadVolume(&volume).IsMalformed());

  redis.set(key.InodeCount(), "not-a-number");
  swordfs::metadata::SwordFsStatFs stat;
  EXPECT_EQ(impl_->StatFs(&stat).code(), Status::kIOError);
}

TEST_F(RedisMetaImplTest, MissingMetadataReturnsNotFoundConsistently) {
  constexpr InodeID kMissingIno = 999999;
  SwordFsInode inode;
  SwordFsAttr attr;
  SwordFsChunk chunk{.index = 0, .start_offset = 0, .size = 1};
  std::string target;
  swordfs::metadata::DirIteratorPtr iterator;

  EXPECT_TRUE(impl_->Lookup(kMissingIno, "x", &inode).IsNotFound());
  EXPECT_TRUE(impl_->GetInode(kMissingIno, &inode).IsNotFound());
  EXPECT_TRUE(impl_->OpenDir(kMissingIno, &iterator).IsNotFound());
  EXPECT_TRUE(impl_->Unlink(kMissingIno, "x", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->RmDir(kMissingIno, "x").IsNotFound());
  EXPECT_TRUE(
      impl_->Rename(kMissingIno, "x", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone, nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Rename(kRootInodeId, "missing", kRootInodeId, "y", swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsNotFound());
  EXPECT_TRUE(impl_->SetAttr(kMissingIno, attr, SetAttrField::kMode, nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Access(kMissingIno, R_OK).IsNotFound());
  EXPECT_TRUE(impl_->Symlink(kMissingIno, "x", "target", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Link(kMissingIno, kRootInodeId, "x", nullptr).IsNotFound());
  EXPECT_TRUE(impl_->Readlink(kMissingIno, &target).IsNotFound());
  EXPECT_TRUE(impl_->Open(kMissingIno).IsNotFound());
  EXPECT_TRUE(impl_->VisitChunks(kMissingIno, [](const SwordFsChunk &) { return Status::OK(); }).IsNotFound());
  EXPECT_TRUE(impl_->AddChunk(kMissingIno, chunk).IsNotFound());
  EXPECT_TRUE(impl_->Truncate(kMissingIno, 1).IsNotFound());
}

TEST_F(RedisMetaImplTest, RenameChecksNewParentAndNonEmptyTargetDirectory) {
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

  EXPECT_TRUE(impl_
                  ->Rename(source_parent.ino, "source", target_parent.ino, "target",
                           swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsNotEmpty());

  SwordFsAttr attr = target_parent.attr;
  attr.mode = S_IFDIR | 0555;
  ASSERT_TRUE(impl_->SetAttr(target_parent.ino, attr, SetAttrField::kMode, nullptr).ok());
  SwordFsContext ctx;
  ctx.uid = 2000;
  ctx.gid = 2000;
  folly::fibers::local<SwordFsContext>() = ctx;
  EXPECT_TRUE(impl_
                  ->Rename(source_parent.ino, "source", target_parent.ino, "moved",
                           swordfs::metadata::RenameFlag::kNone, nullptr)
                  .IsPermission());
}

TEST_F(RedisMetaImplTest, OpenRejectsUnreadableRegularFile) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0600, &file).ok());
  SwordFsContext ctx;
  ctx.uid = 1234;
  ctx.gid = 1234;
  folly::fibers::local<SwordFsContext>() = ctx;
  EXPECT_TRUE(impl_->Open(file.ino).IsPermission());
}

TEST_F(RedisMetaImplTest, LoadVolumeRejectsMissingAndInvalidRoot) {
  RedisMetaConfig config = config_;
  const std::string other_name = volume_name_ + "-unformatted";
  RedisMetaImpl other(config, other_name);
  ASSERT_TRUE(other.Initialize().ok());
  SwordFsVolume volume;
  EXPECT_TRUE(other.LoadVolume(&volume).IsNotFound());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto redis = RawRedis();
  redis.del(key.Inode(kRootInodeId));
  EXPECT_TRUE(impl_->LoadVolume(&volume).IsNotFound());

  SwordFsInode invalid_root;
  invalid_root.ino = kRootInodeId;
  invalid_root.parent_ino = kRootInodeId;
  invalid_root.attr = SwordFsAttr(kRootInodeId, S_IFREG | 0644);
  std::string value;
  ASSERT_TRUE(invalid_root.SerializeTo(&value).ok());
  redis.set(key.Inode(kRootInodeId), value);
  EXPECT_EQ(impl_->LoadVolume(&volume).code(), Status::kInvalidArgument);
}

}  // namespace
