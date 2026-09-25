// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
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

constexpr uint64_t kTestChunkSize = 4096;

swordfs::metadata::PendingDelete MakePendingDelete(InodeID ino, swordfs::metadata::ChunkIndex index,
                                                   swordfs::metadata::ChunkRevision revision) {
  SwordFsChunk descriptor{.index = index, .revision = revision, .size = 64};
  swordfs::metadata::PendingDelete pending;
  const auto status = swordfs::chunk::FreezeWholeObjectDelete(ino, descriptor, kTestChunkSize, &pending);
  EXPECT_TRUE(status.ok()) << status.message();
  return pending;
}

std::string PendingDeleteObjectKey(const swordfs::metadata::PendingDelete &pending) {
  swordfs::chunk::WholeObjectRef ref;
  const auto status = swordfs::chunk::DecodeWholeObjectDelete(pending, kTestChunkSize, &ref);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok() ? ref.key : std::string{};
}

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
    volume_name_ = swordfs::test::UniqueRedisTestNamespace("redis-meta-test");
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

  std::vector<std::string> PendingDeleteKeys(RedisMetaImpl *impl = nullptr) const {
    std::vector<std::string> out;
    auto *target = impl != nullptr ? impl : impl_.get();
    bool has_more = false;
    do {
      auto status = target->VisitPendingDeletesBatch(
          128,
          [&out](const swordfs::metadata::PendingDelete &work) {
            out.push_back(PendingDeleteObjectKey(work));
            return Status::OK();
          },
          &has_more);
      EXPECT_TRUE(status.ok()) << status.message();
      if (!status.ok()) {
        break;
      }
    } while (has_more);
    std::sort(out.begin(), out.end());
    return out;
  }

  void SeedPendingDelete(const swordfs::metadata::PendingDelete &pending) const {
    const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
    std::string encoded;
    ASSERT_TRUE(pending.SerializeTo(&encoded).ok());
    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), pending.id, encoded); });
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

FIBER_TEST_F(RedisMetaImplTest, RenameRejectsTypeMismatchAndDirectoryCycles) {
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
  EXPECT_EQ(
      impl_->Rename(kRootInodeId, "dir", kRootInodeId, "file", swordfs::metadata::RenameFlag::kExchange).ToErrno(),
      EINVAL);

  SwordFsInode child;
  ASSERT_TRUE(impl_->MkDir(dir.ino, "child", 0755, &child).ok());
  EXPECT_EQ(impl_->Rename(kRootInodeId, "dir", child.ino, "moved", swordfs::metadata::RenameFlag::kNone).ToErrno(),
            EINVAL);
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
  EXPECT_EQ(file.attr.mode & (S_ISUID | S_ISGID), 0U);

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
  // still re-apply the inode write side effects instead of returning early.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hset(key.Chunk(file.ino), std::to_string(first.index), replacement_value);
  });

  ASSERT_TRUE(impl_->CommitChunk(file.ino, first, replacement).ok());
  EXPECT_EQ(PendingDeleteKeys(),
            std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(file.ino, first.index, first.revision)});
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

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteScanRejectsFieldValueIdentityMismatch) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto pending = MakePendingDelete(42, 0, 9);
  std::string encoded;
  ASSERT_TRUE(pending.SerializeTo(&encoded).ok());

  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "different/object/key", encoded); });

  bool has_more = false;
  EXPECT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1, [](const swordfs::metadata::PendingDelete &) { return Status::OK(); }, &has_more)
                  .ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteScanRejectsMalformedEnvelopeButLeavesPrivateValidationToStrategy) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "broken", "not-a-record"); });
  bool has_more = false;
  EXPECT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1, [](const swordfs::metadata::PendingDelete &) { return Status::OK(); }, &has_more)
                  .ToErrno() == EIO);

  swordfs::metadata::PendingDelete pending;
  SwordFsChunk invalid_extent{.index = 1, .revision = 9, .size = kTestChunkSize + 1};
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectDelete(42, invalid_extent, 0, &pending).ok());
  std::string encoded;
  ASSERT_TRUE(pending.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.del(key.PendingDeletes());
    redis.hset(key.PendingDeletes(), pending.id, encoded);
  });
  size_t visits = 0;
  EXPECT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1,
                      [&](const swordfs::metadata::PendingDelete &work) {
                        ++visits;
                        swordfs::chunk::WholeObjectRef ref;
                        EXPECT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(work, kTestChunkSize, &ref).ToErrno() ==
                                    EIO);
                        return Status::OK();
                      },
                      &has_more)
                  .ok());
  EXPECT_EQ(visits, 1U);
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

FIBER_TEST_F(RedisMetaImplTest, UnlinkPublishesDurableOrphanCandidate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());

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
  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kTestChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 1));

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

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimIgnoresAdvisoryInodeCountState) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  const std::vector<std::string> counter_states = {"missing", "noncanonical", "wrong-type"};

  for (const auto &counter_state : counter_states) {
    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      redis.del(key.InodeCount());
      redis.set(key.InodeCount(), "1");
    });

    SwordFsInode file;
    ASSERT_TRUE(impl_->Create(kRootInodeId, "file-" + counter_state, 0644, &file).ok()) << counter_state;
    const SwordFsChunk chunk{.index = 0, .revision = 7, .size = 128};
    ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok()) << counter_state;
    ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file-" + counter_state).ok()) << counter_state;

    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      redis.del(key.InodeCount());
      if (counter_state == "noncanonical") {
        redis.set(key.InodeCount(), "02");
      } else if (counter_state == "wrong-type") {
        redis.hset(key.InodeCount(), "wrong", "type");
      }
    });

    std::optional<ReclaimWork> work;
    const auto status = impl_->PrepareReclaim(file.ino, &work);
    ASSERT_TRUE(status.ok()) << counter_state << ": " << status.message();
    ASSERT_TRUE(work.has_value()) << counter_state;
    EXPECT_EQ(work->ino, file.ino) << counter_state;
    EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound()) << counter_state;

    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino))) << counter_state;
    });

    swordfs::metadata::SwordFsStatFs stat;
    ASSERT_TRUE(impl_->StatFs(&stat).ok()) << counter_state;
    EXPECT_EQ(stat.files, impl_->GetLimits().max_free_inodes) << counter_state;
    EXPECT_EQ(stat.files_free, stat.files) << counter_state;
    ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok()) << counter_state;
  }
}

FIBER_TEST_F(RedisMetaImplTest, CrashLeftPendingReclaimIsRetriedFromPersistedState) {
  // Model a crash between "prepared" and "completed": the frozen record is in
  // Redis and the inode is gone. Recovery must hand back the same frozen
  // identities — nothing may be re-derived from live state (there is none).
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .revision = 3, .size = 128};
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
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*retried, kTestChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 3));

  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  ASSERT_TRUE(impl_->VisitPendingReclaims([](const ReclaimWork &) { return Status::OK(); }).ok());
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimRejectsFrozenRecordWithLiveInode) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "inconsistent-reclaim", 0644, &file).ok());
  const SwordFsChunk head{.index = 0, .revision = 7, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, head).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "inconsistent-reclaim").ok());

  ReclaimWork frozen;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(file.ino, {head}, kTestChunkSize, &frozen).ok());
  std::string encoded;
  ASSERT_TRUE(frozen.SerializeTo(&encoded).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    // This state is outside the current one-stage beta protocol. Reclaim must
    // fail closed rather than inventing compatibility recovery for it.
    redis.hset(key.Reclaims(), std::to_string(file.ino), encoded);
    EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
    EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
  });

  std::optional<ReclaimWork> replay;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &replay).ToErrno() == EBUSY);
  EXPECT_FALSE(replay.has_value());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
    EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
    EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
  });
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

  ReclaimWork stale;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(file.ino, {}, kTestChunkSize, &stale).ok());
  std::string encoded;
  ASSERT_TRUE(stale.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), std::to_string(file.ino), encoded); });

  std::optional<ReclaimWork> work;
  // A frozen record alongside a live inode is outside the current one-stage
  // protocol. Never reinterpret or cancel it while the inode is live.
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ToErrno() == EBUSY);
  EXPECT_FALSE(work.has_value());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });
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
                  .ToErrno() == EIO);

  // PrepareReclaim replays an existing frozen record before consulting live
  // inode state. A corrupt pending record must therefore fail closed here too
  // rather than being treated as a fresh reclaim.
  std::optional<ReclaimWork> replay;
  EXPECT_TRUE(impl_->PrepareReclaim(4242, &replay).ToErrno() == EIO);
  EXPECT_FALSE(replay.has_value());

  // A decodable record whose serialized inode disagrees with its hash field
  // is corrupt too: acting on it could delete another inode's objects.
  ReclaimWork record;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(7777, {}, kTestChunkSize, &record).ok());
  std::string serialized;
  ASSERT_TRUE(record.SerializeTo(&serialized).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), "4242", serialized); });
  EXPECT_TRUE(impl_->VisitPendingReclaims([](const ReclaimWork &) { return Status::OK(); }).ToErrno() == EIO);

  // A key that is not an inode id at all is not a candidate either.
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Reclaims(), "4242");
    redis.hset(key.Orphans(), "not-an-inode", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).ToErrno() == EIO);

  // Exercise the other field-parsing failures independently: a numeric
  // prefix with trailing junk, and the reserved zero inode id.
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Orphans(), "not-an-inode");
    redis.hset(key.Orphans(), "42x", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).ToErrno() == EIO);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Orphans(), "42x");
    redis.hset(key.Orphans(), "0", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimRejectsPendingRecordForAnotherInode) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  ReclaimWork wrong;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(file.ino + 1, {}, kTestChunkSize, &wrong).ok());
  std::string serialized;
  ASSERT_TRUE(wrong.SerializeTo(&serialized).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), std::to_string(file.ino), serialized); });

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ToErrno() == EIO);
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
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ToErrno() == EIO);
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
  EXPECT_EQ(impl_->VisitOrphanCandidates(swordfs::metadata::InodeVisitorFn{}).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->VisitPendingReclaims(swordfs::metadata::ReclaimVisitorFn{}).ToErrno(), EINVAL);
  bool has_more = false;
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(0, [](const auto &) { return Status::OK(); }, &has_more).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, swordfs::metadata::PendingDeleteVisitorFn{}, &has_more).ToErrno(),
            EINVAL);
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, [](const auto &) { return Status::OK(); }, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->PrepareReclaim(kRootInodeId, nullptr).ToErrno(), EINVAL);

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
  EXPECT_EQ(orphan_abort.ToErrno(), EBUSY);
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
  EXPECT_EQ(pending_abort.ToErrno(), EIO);
  EXPECT_EQ(visited, (std::vector<InodeID>{second.ino}));
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchBoundsVisitsAndContinuesWhileQueueMutates) {
  constexpr size_t kRecordCount = 7;
  std::vector<std::string> expected;
  for (size_t i = 0; i < kRecordCount; ++i) {
    auto pending = MakePendingDelete(42, static_cast<swordfs::metadata::ChunkIndex>(i), i + 1);
    expected.push_back(PendingDeleteObjectKey(pending));
    SeedPendingDelete(pending);
  }
  std::sort(expected.begin(), expected.end());

  std::vector<std::string> visited;
  bool has_more = true;
  size_t calls = 0;
  while (has_more && calls++ < 32) {
    size_t visits_this_call = 0;
    auto status = impl_->VisitPendingDeletesBatch(
        2,
        [&](const swordfs::metadata::PendingDelete &work) {
          ++visits_this_call;
          visited.push_back(PendingDeleteObjectKey(work));
          return impl_->CompletePendingDelete(work.id);
        },
        &has_more);
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_LE(visits_this_call, 2U);
  }
  EXPECT_LT(calls, 32U);
  EXPECT_FALSE(has_more);
  std::sort(visited.begin(), visited.end());
  visited.erase(std::unique(visited.begin(), visited.end()), visited.end());
  EXPECT_EQ(visited, expected);
  EXPECT_TRUE(PendingDeleteKeys().empty());
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchUncompletedIntentDoesNotStarveLaterRecords) {
  constexpr size_t kRecordCount = 7;
  auto live = MakePendingDelete(42, 0, 1);
  SeedPendingDelete(live);
  for (size_t i = 1; i < kRecordCount; ++i) {
    SeedPendingDelete(MakePendingDelete(42, static_cast<swordfs::metadata::ChunkIndex>(i), i + 1));
  }

  bool has_more = true;
  size_t calls = 0;
  while (has_more && calls++ < 64) {
    auto status = impl_->VisitPendingDeletesBatch(
        1,
        [&](const swordfs::metadata::PendingDelete &work) {
          if (PendingDeleteObjectKey(work) == PendingDeleteObjectKey(live)) {
            return Status::OK();
          }
          return impl_->CompletePendingDelete(work.id);
        },
        &has_more);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  EXPECT_LT(calls, 64U);
  EXPECT_FALSE(has_more);
  EXPECT_EQ(PendingDeleteKeys(), std::vector<std::string>{PendingDeleteObjectKey(live)});
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchVisitorAbortRetriesCurrentBufferedRecord) {
  auto pending = MakePendingDelete(42, 0, 1);
  SeedPendingDelete(pending);

  bool has_more = false;
  std::string first_key;
  auto status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &work) {
        first_key = PendingDeleteObjectKey(work);
        return Status::Busy("stop bounded pending delete scan");
      },
      &has_more);
  EXPECT_TRUE(status.ToErrno() == EBUSY) << status.message();
  EXPECT_EQ(first_key, PendingDeleteObjectKey(pending));

  std::string retried_key;
  status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &work) {
        retried_key = PendingDeleteObjectKey(work);
        return Status::OK();
      },
      &has_more);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(retried_key, first_key);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchMalformedRecordFailsBeforeVisitor) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "broken", "not-a-pending-delete"); });

  bool has_more = false;
  size_t visits = 0;
  const auto status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &) {
        ++visits;
        return Status::OK();
      },
      &has_more);
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
  EXPECT_EQ(visits, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchRestartRediscoversDurableRemainder) {
  auto first = MakePendingDelete(42, 0, 1);
  auto second = MakePendingDelete(42, 1, 2);
  SeedPendingDelete(first);
  SeedPendingDelete(second);

  bool has_more = false;
  size_t visits = 0;
  ASSERT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1,
                      [&](const swordfs::metadata::PendingDelete &) {
                        ++visits;
                        return Status::OK();
                      },
                      &has_more)
                  .ok());
  EXPECT_EQ(visits, 1U);
  EXPECT_TRUE(has_more);

  std::unique_ptr<RedisMetaImpl> peer;
  swordfs::test::RunInTestThreadFromFiber([&] {
    peer = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    ASSERT_TRUE(peer->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    ASSERT_TRUE(peer->LoadVolume(&volume).ok());
  });

  std::vector<std::string> rediscovered;
  has_more = true;
  size_t calls = 0;
  while (has_more && calls++ < 16) {
    auto status = peer->VisitPendingDeletesBatch(
        1,
        [&](const swordfs::metadata::PendingDelete &work) {
          rediscovered.push_back(PendingDeleteObjectKey(work));
          return peer->CompletePendingDelete(work.id);
        },
        &has_more);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  EXPECT_LT(calls, 16U);
  std::sort(rediscovered.begin(), rediscovered.end());
  rediscovered.erase(std::unique(rediscovered.begin(), rediscovered.end()), rediscovered.end());
  EXPECT_EQ(rediscovered.size(), 2U);
  EXPECT_TRUE(PendingDeleteKeys(peer.get()).empty());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

}  // namespace
