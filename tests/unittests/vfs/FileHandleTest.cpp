// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileHandle — the fh → FileHandle mapping and the
// high-level Open/Create entry points.

#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "TestWatchdog.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/cow/COWCleanup.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"
#include "vfs/DirHandle.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeFlag;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::utils::Status;

// Minimal no-op data engine used by generic FileHandle tests.
class NoopDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return put_status;
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }

  Status put_status = Status::OK();
};

// Minimal IMetaEngine — every op succeeds; Create fabricates an inode.
class MockMetaEngine : public IMetaEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status FormatVolume(const SwordFsVolume &) override {
    return Status::OK();
  }
  Status LoadVolume(SwordFsVolume *) override {
    return Status::OK();
  }
  Limits GetLimits() const override {
    return {};
  }
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID, SwordFsInode *out) override {
    // The reclaim path no longer consults inode metadata; this remains for
    // tests that model an inode's attributes (ownership, nlink).
    if (out) {
      *out = {};
    }
    return Status::OK();
  }
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("inode batch output is null");
    }
    out->clear();
    for (const InodeID requested_ino : inode_ids) {
      SwordFsInode inode;
      auto status = GetInode(requested_ino, &inode);
      if (!status.ok()) {
        return status;
      }
      out->emplace_back(std::move(inode));
    }
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = next_ino_++;
      out->attr.ino = out->ino;
      out->attr.mode = S_IFREG | 0644;
    }
    return Status::OK();
  }
  Status MkNod(InodeID, std::string_view, uint32_t, uint64_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) override {
    return Status::OK();
  }
  Status SetXAttr(InodeID, std::string_view, std::string_view, swordfs::metadata::XAttrSetMode) override {
    return Status::NotSupported("xattr");
  }
  Status GetXAttr(InodeID, std::string_view, std::string *) override {
    return Status::NotSupported("xattr");
  }
  Status ListXAttrs(InodeID, std::vector<std::string> *) override {
    return Status::NotSupported("xattr");
  }
  Status RemoveXAttr(InodeID, std::string_view) override {
    return Status::NotSupported("xattr");
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID, uint64_t *size = nullptr, InodeFlag *inode_flags = nullptr) override {
    const auto status = open_status;
    const auto inode_flags_snapshot = open_inode_flags;
    if (open_entered_ != nullptr) {
      auto *entered = open_entered_;
      auto *release = open_release_;
      open_entered_ = nullptr;
      open_release_ = nullptr;
      entered->post();
      release->wait();
    }
    if (size != nullptr) {
      *size = 0;
    }
    if (inode_flags != nullptr) {
      *inode_flags = inode_flags_snapshot;
    }
    return status;
  }
  Status SetInodeFlags(InodeID, InodeFlag inode_flags, SwordFsInode *out) override {
    ++set_inode_flags_calls;
    commit_chunk_calls_at_flag_set = commit_chunk_calls;
    last_inode_flags = inode_flags;
    open_inode_flags = inode_flags;
    if (out != nullptr) {
      *out = {};
      out->attr.inode_flags = inode_flags;
    }
    return set_inode_flags_status;
  }
  Status PrepareReclaim(InodeID ino) override {
    ++prepare_reclaim_calls;
    last_reclaim_ino = ino;
    return prepare_reclaim_status;
  }
  Status CompleteReclaim(InodeID ino) override {
    ++reclaim_calls;
    last_reclaim_ino = ino;
    return reclaim_status;
  }
  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingDeletesBatch(size_t, const swordfs::metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }
  Status CompletePendingDelete(std::string_view) override {
    return Status::OK();
  }
  Status AllocateChunkRevision(swordfs::metadata::ChunkRevision *revision) override {
    if (revision == nullptr) {
      return Status::InvalidArgument("chunk revision output is null");
    }
    *revision = next_revision_++;
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status CommitChunk(InodeID, const std::optional<SwordFsChunk> &, const SwordFsChunk &) override {
    ++commit_chunk_calls;
    return Status::OK();
  }
  Status FindChunk(InodeID, ChunkIndex, SwordFsChunk *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID, uint64_t size) override {
    ++truncate_calls;
    last_truncate_size = size;
    return truncate_status;
  }

  // Observable state / injectable statuses for tests.
  int truncate_calls = 0;
  size_t last_truncate_size = 0;
  // Number of completed reclaims (CompleteReclaim calls).
  int reclaim_calls = 0;
  int prepare_reclaim_calls = 0;
  InodeID last_reclaim_ino = 0;
  Status open_status = Status::OK();
  InodeFlag open_inode_flags = InodeFlag::kNone;
  InodeFlag last_inode_flags = InodeFlag::kNone;
  int set_inode_flags_calls = 0;
  int commit_chunk_calls = 0;
  int commit_chunk_calls_at_flag_set = 0;
  Status set_inode_flags_status = Status::OK();
  Status truncate_status = Status::OK();
  Status reclaim_status = Status::OK();
  Status prepare_reclaim_status = Status::OK();
  // Frozen descriptors the mock hands out from PrepareReclaim.
  std::vector<swordfs::metadata::SwordFsChunk> reclaim_chunks;

  void BlockNextOpen(folly::fibers::Baton *entered, folly::fibers::Baton *release) {
    open_entered_ = entered;
    open_release_ = release;
  }

 private:
  InodeID next_ino_ = 1000;
  swordfs::metadata::ChunkRevision next_revision_ = 1;
  folly::fibers::Baton *open_entered_{nullptr};
  folly::fibers::Baton *open_release_{nullptr};
};

class FileHandleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<MockMetaEngine>>();
    mock_meta_ = meta.get();
    auto data = std::make_unique<NoopDataEngine>();
    data_ = data.get();
    SwordFsVolume config;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] {
      // Drop any per-inode state left by a prior test. The
      // InodeHandleManager is fiber-owned runtime state.
      InodeHandleManager::Instance().Initialize();
    });
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] {
      // Clean up any handles left by a test.
      for (uint64_t fh : fhs_) {
        if (auto handle = HandleManager::Instance().FindAs<FileHandle>(fh)) {
          handle->Release();
        } else if (auto handle = HandleManager::Instance().FindAs<DirHandle>(fh)) {
          handle->Release();
        }
      }
    });
    // Destroy/reset injected engines on the POSIX test thread.
    volume::VolumeImpl::Initialize();
  }

  /// Helper: open a handle and remember it for cleanup.
  uint64_t OpenHandle(metadata::InodeID ino) {
    std::shared_ptr<FileHandle> handle;
    auto status = FileHandle::Open(ino, 0, &handle);
    EXPECT_TRUE(status.ok());
    uint64_t fh = handle->fh();
    fhs_.push_back(fh);
    return fh;
  }

  std::vector<uint64_t> fhs_;
  MockMetaEngine *mock_meta_ = nullptr;
  NoopDataEngine *data_ = nullptr;
};

// ────────────────────────────────────────────────────────────────
// Singleton
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, InstanceIsSingleton) {
  auto &a = HandleManager::Instance();
  auto &b = HandleManager::Instance();
  EXPECT_EQ(&a, &b);
}

// ────────────────────────────────────────────────────────────────
// Open + Find
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, OpenAndFind) {
  uint64_t fh = OpenHandle(42);

  auto found = HandleManager::Instance().FindAs<FileHandle>(fh);
  EXPECT_NE(found, nullptr);
}

FIBER_TEST_F(FileHandleTest, FindNonexistent) {
  auto found = HandleManager::Instance().FindAs<FileHandle>(999);
  EXPECT_EQ(found, nullptr);
}

FIBER_TEST_F(FileHandleTest, OpenMultipleHandles) {
  uint64_t fh1 = OpenHandle(10);
  uint64_t fh2 = OpenHandle(20);
  EXPECT_NE(fh1, fh2);

  auto f1 = HandleManager::Instance().FindAs<FileHandle>(fh1);
  auto f2 = HandleManager::Instance().FindAs<FileHandle>(fh2);

  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);
  EXPECT_NE(InodeHandleManager::Instance().Get(10, false).get(), InodeHandleManager::Instance().Get(20, false).get());
}

// ────────────────────────────────────────────────────────────────
// HandleManager unregister
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, ReleaseRemovesHandle) {
  uint64_t fh = OpenHandle(7);
  auto handle = HandleManager::Instance().FindAs<FileHandle>(fh);
  ASSERT_NE(handle, nullptr);
  ASSERT_TRUE(handle->Release().ok());

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh), nullptr);
}

FIBER_TEST_F(FileHandleTest, ReleaseKeepsOtherHandles) {
  uint64_t fh1 = OpenHandle(1);
  uint64_t fh2 = OpenHandle(2);
  auto f1 = HandleManager::Instance().FindAs<FileHandle>(fh1);
  ASSERT_NE(f1, nullptr);
  ASSERT_TRUE(f1->Release().ok());

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh1), nullptr);
  auto f2 = HandleManager::Instance().FindAs<FileHandle>(fh2);
  ASSERT_NE(f2, nullptr);
}

// ────────────────────────────────────────────────────────────────
// Open duplicate fh
// ────────────────────────────────────────────────────────────────

// With auto-allocated fh, duplicates cannot occur — every Open gets a
// unique handle.
FIBER_TEST_F(FileHandleTest, OpenReturnsUniqueFh) {
  uint64_t fh1 = OpenHandle(100);
  uint64_t fh2 = OpenHandle(200);
  EXPECT_NE(fh1, fh2);
}

// ────────────────────────────────────────────────────────────────
// Shared ownership — Find keeps handle alive across Unregister
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, FindKeepsHandleAliveAfterUnregister) {
  uint64_t fh = OpenHandle(55);

  // Hold a shared_ptr before releasing.
  auto held = HandleManager::Instance().FindAs<FileHandle>(fh);
  ASSERT_NE(held, nullptr);

  ASSERT_TRUE(held->Release().ok());
  // Map entry is gone.
  auto after = HandleManager::Instance().FindAs<FileHandle>(fh);
  EXPECT_EQ(after, nullptr);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->fh(), fh);  // the held shared_ptr still owns the FileHandle
}

// ────────────────────────────────────────────────────────────────
// Concurrency — basic multi-threaded access
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, ConcurrentOpenAndFind) {
  constexpr int kThreads = 4;
  constexpr int kIters = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.push_back(swordfs::test::StartFiberTestThread([t] {
      for (int i = 0; i < kIters; ++i) {
        auto ino = static_cast<metadata::InodeID>(t * kIters + i + 100);
        std::shared_ptr<FileHandle> handle;
        auto status = FileHandle::Open(ino, 0, &handle);
        EXPECT_TRUE(status.ok());
        auto found = HandleManager::Instance().FindAs<FileHandle>(handle->fh());
        EXPECT_NE(found, nullptr);
        // Release eagerly: leaving 400 fh dangling across the test
        // boundary corrupts later open-count assertions (Get(ino, false)
        // would hand out handles with a non-zero open_count for these
        // synthetic inodes).
        ASSERT_TRUE(handle->Release().ok());
      }
    }));
  }
  for (auto &th : threads) {
    // These workers have a finite loop and no test-controlled blocking
    // barrier. #361 only bounds joins whose completion depends on a deliberate
    // deadlock/ordering scenario.
    th.join();
  }
}

class TestDirIterator final : public metadata::DirIterator {
 public:
  Status Seek(uint64_t) override {
    return Status::OK();
  }

  Status Peek(metadata::SwordFsEntry *, uint64_t *) override {
    return Status::EndOfDirectory("directory end");
  }

  void Advance() override {
  }
};

metadata::DirIteratorPtr NewTestDirIterator() {
  return std::make_shared<TestDirIterator>();
}

// ────────────────────────────────────────────────────────────────
// Generic file and directory handles
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, RegisterAndFindDirectoryHandle) {
  auto handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t fh = HandleManager::Instance().Register(handle);

  auto found = HandleManager::Instance().FindAs<DirHandle>(fh);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found, handle);
  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, RegisterAssignsUniqueHandlesAcrossTypes) {
  std::shared_ptr<FileHandle> opened_file;
  ASSERT_TRUE(FileHandle::Open(9007, 0, &opened_file).ok());
  auto file_handle = HandleManager::Instance().FindAs<FileHandle>(opened_file->fh());
  ASSERT_NE(file_handle, nullptr);

  auto dir_handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t dir_fh = HandleManager::Instance().Register(dir_handle);

  EXPECT_NE(opened_file->fh(), dir_fh);
  EXPECT_EQ(file_handle->fh(), opened_file->fh());
  EXPECT_EQ(dir_handle->fh(), dir_fh);

  ASSERT_TRUE(opened_file->Release().ok());
  ASSERT_TRUE(dir_handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, FindAsRejectsWrongHandleType) {
  auto handle = std::make_shared<DirHandle>(NewTestDirIterator());
  const uint64_t fh = HandleManager::Instance().Register(handle);

  EXPECT_EQ(HandleManager::Instance().FindAs<FileHandle>(fh), nullptr);
  EXPECT_NE(HandleManager::Instance().FindAs<DirHandle>(fh), nullptr);
  ASSERT_TRUE(handle->Release().ok());
}

// ────────────────────────────────────────────────────────────────
// FileHandle::Open error propagation
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, OpenMetaFailurePropagates) {
  mock_meta_->open_status = Status::Permission("denied");
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, 0, &handle);
  EXPECT_TRUE(status.ToErrno() == EACCES);
}

FIBER_TEST_F(FileHandleTest, OpenTruncateAppliesOTrunc) {
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  ASSERT_TRUE(status.ok());
  fhs_.push_back(handle->fh());
  EXPECT_EQ(mock_meta_->truncate_calls, 1);
  EXPECT_EQ(mock_meta_->last_truncate_size, 0u);
}

FIBER_TEST_F(FileHandleTest, OpenTruncateFailurePropagates) {
  mock_meta_->truncate_status = Status::Internal("truncate failed");
  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Open(42, O_TRUNC, &handle);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.ToErrno(), EIO);
}

FIBER_TEST_F(FileHandleTest, OpenEnforcesImmutableAndAppendOnlyPolicyFromSameMetadataRead) {
  mock_meta_->open_inode_flags = InodeFlag::kImmutable;
  std::shared_ptr<FileHandle> immutable;
  EXPECT_EQ(FileHandle::Open(42, O_WRONLY, &immutable).ToErrno(), EPERM);
  std::shared_ptr<FileHandle> immutable_read_only;
  ASSERT_TRUE(FileHandle::Open(42, O_RDONLY, &immutable_read_only).ok());
  fhs_.push_back(immutable_read_only->fh());

  mock_meta_->open_inode_flags = InodeFlag::kAppendOnly;
  std::shared_ptr<FileHandle> append_without_flag;
  EXPECT_EQ(FileHandle::Open(42, O_WRONLY, &append_without_flag).ToErrno(), EPERM);
  std::shared_ptr<FileHandle> append_read_only;
  ASSERT_TRUE(FileHandle::Open(42, O_RDONLY, &append_read_only).ok());
  fhs_.push_back(append_read_only->fh());

  std::shared_ptr<FileHandle> append;
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY | O_APPEND, &append).ok());
  fhs_.push_back(append->fh());

  std::shared_ptr<FileHandle> truncating_append;
  EXPECT_EQ(FileHandle::Open(42, O_WRONLY | O_APPEND | O_TRUNC, &truncating_append).ToErrno(), EPERM);
}

FIBER_TEST_F(FileHandleTest, SameMountFlagTransitionRevokesPreexistingHandlesAtInodeOperationBoundary) {
  mock_meta_->open_inode_flags = InodeFlag::kNone;
  std::shared_ptr<FileHandle> ordinary;
  std::shared_ptr<FileHandle> append;
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY, &ordinary).ok());
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY | O_APPEND, &append).ok());
  fhs_.push_back(ordinary->fh());
  fhs_.push_back(append->fh());

  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kImmutable, nullptr).ok());

  auto payload = folly::IOBuf::copyBuffer("x", 1);
  EXPECT_EQ(ordinary->Write(*payload, 0).ToErrno(), EPERM);
  EXPECT_EQ(append->Write(*payload, 0).ToErrno(), EPERM);

  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kAppendOnly, nullptr).ok());
  EXPECT_EQ(ordinary->Write(*payload, 0).ToErrno(), EPERM);
  EXPECT_TRUE(append->Write(*payload, 0).ok());

  // Idempotent writes and clearing policy both refresh metadata/local state
  // without a restrictive-transition flush.
  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kAppendOnly, nullptr).ok());
  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kNone, nullptr).ok());
  EXPECT_TRUE(ordinary->Write(*payload, 0).ok());
}

FIBER_TEST_F(FileHandleTest, ConcurrentOpenCannotRestoreStalePolicyAfterLocalFlagTransition) {
  mock_meta_->open_inode_flags = InodeFlag::kNone;
  std::shared_ptr<FileHandle> first;
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY, &first).ok());
  fhs_.push_back(first->fh());

  folly::fibers::Baton open_entered;
  folly::fibers::Baton open_release;
  folly::fibers::Baton opening_thread_done;
  mock_meta_->BlockNextOpen(&open_entered, &open_release);

  std::atomic<int> open_code{0};
  std::shared_ptr<FileHandle> second;
  std::thread opening_thread([&] {
    swordfs::test::RunInTestFiber([&] {
      const auto status = FileHandle::Open(42, O_WRONLY, &second);
      open_code.store(status.ToErrno());
    });
    opening_thread_done.post();
  });

  const bool open_was_entered = swordfs::test::WaitForBaton(open_entered);
  EXPECT_TRUE(open_was_entered) << "concurrent Open must capture the controlled stale metadata snapshot";
  if (!open_was_entered) {
    open_release.post();
    swordfs::test::WaitForBatonOrAbort(opening_thread_done, "concurrent Open timeout cleanup");
    opening_thread.join();
    return;
  }

  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/false);
  EXPECT_NE(inode_handle, nullptr);
  if (inode_handle == nullptr) {
    open_release.post();
    swordfs::test::WaitForBatonOrAbort(opening_thread_done, "concurrent Open missing-inode cleanup");
    opening_thread.join();
    return;
  }
  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kImmutable, nullptr).ok());

  open_release.post();
  swordfs::test::WaitForBatonOrAbort(opening_thread_done, "concurrent Open completion after flag transition");
  opening_thread.join();

  EXPECT_EQ(open_code.load(), EPERM);
  EXPECT_EQ(second, nullptr);
  auto payload = folly::IOBuf::copyBuffer("x");
  EXPECT_EQ(first->Write(*payload, 0).ToErrno(), EPERM);

  if (second != nullptr) {
    second->Release();
  }
}

FIBER_TEST_F(FileHandleTest, CreateInitializationCannotOverwriteNewerLocalPolicy) {
  mock_meta_->open_inode_flags = InodeFlag::kNone;
  std::shared_ptr<FileHandle> opener;
  ASSERT_TRUE(FileHandle::Open(42, O_RDONLY, &opener).ok());
  fhs_.push_back(opener->fh());

  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kImmutable, nullptr).ok());

  // A concurrent CREATE may reach handle initialization after another local
  // request has already committed a restrictive policy on the shared inode
  // handle. The CREATE's original kNone snapshot must not restore stale state.
  std::shared_ptr<FileHandle> created;
  ASSERT_TRUE(FileHandle::Create(42, O_WRONLY, &created).ok());
  fhs_.push_back(created->fh());

  auto payload = folly::IOBuf::copyBuffer("x");
  EXPECT_EQ(created->Write(*payload, 0).ToErrno(), EPERM);
}

FIBER_TEST_F(FileHandleTest, RestrictiveFlagTransitionFlushesAcceptedDirtyDataBeforeMetadataCommit) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY, &handle).ok());
  fhs_.push_back(handle->fh());

  auto payload = folly::IOBuf::copyBuffer("dirty-before-flag");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());
  ASSERT_EQ(mock_meta_->commit_chunk_calls, 0);

  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->SetInodeFlags(InodeFlag::kImmutable, nullptr).ok());

  EXPECT_EQ(mock_meta_->commit_chunk_calls, 1);
  EXPECT_EQ(mock_meta_->commit_chunk_calls_at_flag_set, 1);
  EXPECT_EQ(mock_meta_->set_inode_flags_calls, 1);
}

FIBER_TEST_F(FileHandleTest, FailedDirtyFlushDoesNotCommitRestrictivePolicy) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY, &handle).ok());
  fhs_.push_back(handle->fh());

  auto payload = folly::IOBuf::copyBuffer("dirty-before-failed-flag");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());
  data_->put_status = Status::IOError("injected flag-transition flush failure");

  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->SetInodeFlags(InodeFlag::kImmutable, nullptr).ToErrno(), EIO);
  EXPECT_EQ(mock_meta_->set_inode_flags_calls, 0);

  data_->put_status = Status::OK();
  auto later = folly::IOBuf::copyBuffer("still-writable");
  EXPECT_TRUE(handle->Write(*later, static_cast<off_t>(payload->length())).ok());
}

FIBER_TEST_F(FileHandleTest, InodeFlagMutationRejectsInvalidBitsAndKeepsLocalPolicyOnMetadataFailure) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY, &handle).ok());
  fhs_.push_back(handle->fh());

  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->SetInodeFlags(static_cast<InodeFlag>(1u << 31), nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(mock_meta_->set_inode_flags_calls, 0);

  mock_meta_->set_inode_flags_status = Status::IOError("injected flag commit failure");
  EXPECT_EQ(inode_handle->SetInodeFlags(InodeFlag::kImmutable, nullptr).ToErrno(), EIO);
  EXPECT_EQ(mock_meta_->set_inode_flags_calls, 1);

  auto payload = folly::IOBuf::copyBuffer("still-writable-after-failed-policy-commit");
  EXPECT_TRUE(handle->Write(*payload, 0).ok());
}

FIBER_TEST_F(FileHandleTest, CreateRejectsNullOutput) {
  auto status = FileHandle::Create(42, O_RDWR, nullptr);
  EXPECT_EQ(status.ToErrno(), EINVAL);
}

FIBER_TEST_F(FileHandleTest, CreateRespectsReclaimFence) {
  auto inode_handle = InodeHandleManager::Instance().Get(42, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->TryStartReclaim());

  std::shared_ptr<FileHandle> handle;
  auto status = FileHandle::Create(42, O_RDWR, &handle);
  EXPECT_TRUE(status.IsNotFound()) << status.message();
  EXPECT_EQ(handle, nullptr);

  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, AccessModeIsPerFileHandle) {
  std::shared_ptr<FileHandle> read_only;
  std::shared_ptr<FileHandle> write_only;
  ASSERT_TRUE(FileHandle::Open(42, O_RDONLY, &read_only).ok());
  ASSERT_TRUE(FileHandle::Open(42, O_WRONLY, &write_only).ok());
  fhs_.push_back(read_only->fh());
  fhs_.push_back(write_only->fh());

  EXPECT_FALSE(read_only->writable());
  EXPECT_TRUE(write_only->writable());
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, InodeHandleGetMissingWithoutCreate) {
  EXPECT_EQ(InodeHandleManager::Instance().Get(9001, /*create_if_missing=*/false), nullptr);
}

FIBER_TEST_F(FileHandleTest, InodeHandleGetExistingReflectsLiveDescriptor) {
  OpenHandle(9002);
  auto inode_handle = InodeHandleManager::Instance().Get(9002, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_FALSE(inode_handle->TryStartReclaim());
}

FIBER_TEST_F(FileHandleTest, InodeHandleRecreatedAfterExpiry) {
  {
    auto inode_handle = InodeHandleManager::Instance().Get(9003, true);
    ASSERT_NE(inode_handle, nullptr);
  }
  // All shared references dropped — the weak entry is expired, so a fresh
  // InodeHandle must be created on the next lookup.
  auto recreated = InodeHandleManager::Instance().Get(9003, true);
  ASSERT_NE(recreated, nullptr);
}

// ────────────────────────────────────────────────────────────────
// InodeHandle open-reference / reclaim-fence contract
// ────────────────────────────────────────────────────────────────

FIBER_TEST_F(FileHandleTest, ReclaimFenceRefusesWhileDescriptorIsOpenAndAllowsAfterClose) {
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(9004, 0, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9004, false);
  ASSERT_NE(inode_handle, nullptr);

  EXPECT_FALSE(inode_handle->TryStartReclaim()) << "a live descriptor must block reclaim preparation";
  ASSERT_TRUE(handle->Release().ok());

  ASSERT_TRUE(inode_handle->TryStartReclaim());
  std::shared_ptr<FileHandle> blocked;
  EXPECT_TRUE(FileHandle::Open(9004, O_RDONLY, &blocked).IsNotFound())
      << "an open must not cross a claimed reclaim fence";
  inode_handle->FinishReclaim();

  std::shared_ptr<FileHandle> reopened;
  ASSERT_TRUE(FileHandle::Open(9004, O_RDONLY, &reopened).ok());
  ASSERT_TRUE(reopened->Release().ok());
}

FIBER_TEST_F(FileHandleTest, CloseOnlyReleasesItsOwnReference) {
  std::shared_ptr<FileHandle> h1, h2;
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h1).ok());
  ASSERT_TRUE(FileHandle::Open(9005, 0, &h2).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(9005, false);
  ASSERT_NE(inode_handle, nullptr);

  // Close only owns descriptor accounting. Durable orphan work and object
  // deletion belong to OrphanReclaimer / private chunk GC, so no metadata reclaim call is made here.
  ASSERT_TRUE(h1->Release().ok());
  EXPECT_FALSE(inode_handle->TryStartReclaim());

  ASSERT_TRUE(h2->Release().ok());
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

// ────────────────────────────────────────────────────────────────
// InodeHandle open-fd lifecycle
// ────────────────────────────────────────────────────────────────
//
// OrphanReclaimer observes descriptor liveness through
// TryStartReclaim(). The case below exercises no handle -> open -> reclaim
// blocked -> close -> reclaim allowed without exposing the internal counter.

FIBER_TEST_F(FileHandleTest, InodeHandleOpenFdLifecycleControlsReclaim) {
  // No handle yet -> Get without create reports absence.
  InodeID test_ino = 9998;
  EXPECT_EQ(InodeHandleManager::Instance().Get(test_ino, false), nullptr);

  // FileHandle::Open routes through mock_meta_ and creates the handle.
  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(test_ino, O_RDWR, &handle).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(test_ino, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_FALSE(inode_handle->TryStartReclaim());

  // Release drops the descriptor reference so reclaim may claim its fence.
  ASSERT_TRUE(handle->Release().ok());
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

// ────────────────────────────────────────────────────────────────
// InodeHandle runtime-state test fixtures
// ────────────────────────────────────────────────────────────────
//
// These fakes support open/flush/fence lifecycle tests. Durable reclaim
// sequencing itself is owned and tested by ReclaimerTest.

namespace {

// Mock data engine: records every Delete call and lets the test inject
// per-key failure responses.
class FakeDataEngine : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    if (put_entered_ != nullptr) {
      auto *entered = put_entered_;
      auto *release = put_release_;
      put_entered_ = nullptr;
      put_release_ = nullptr;
      entered->post();
      release->wait();
    }
    return put_status;
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view key) override {
    delete_calls.push_back(std::string(key));
    auto it = fail_keys.find(std::string(key));
    if (it != fail_keys.end()) {
      return it->second;
    }
    return Status::OK();
  }

  void BlockNextPut(folly::fibers::Baton *entered, folly::fibers::Baton *release) {
    put_entered_ = entered;
    put_release_ = release;
  }

  std::vector<std::string> delete_calls;
  std::unordered_map<std::string, Status> fail_keys;
  Status put_status = Status::OK();

 private:
  folly::fibers::Baton *put_entered_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
};

// Mock metadata engine with configurable Open/Prepare behaviour for runtime
// handle lifecycle tests.
class TrackingMetaEngine : public swordfs::metadata::IMetaEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status FormatVolume(const SwordFsVolume &) override {
    return Status::OK();
  }
  Status LoadVolume(SwordFsVolume *) override {
    return Status::OK();
  }
  Limits GetLimits() const override {
    return {};
  }
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    ++get_inode_calls;
    auto it = attrs.find(ino);
    if (it == attrs.end()) {
      return Status::NotFound("inode not found");
    }
    if (out) {
      *out = {};
      out->ino = ino;
      out->attr.ino = it->second.st_ino;
      out->attr.mode = it->second.st_mode;
      out->attr.nlink = it->second.st_nlink;
      out->attr.uid = it->second.st_uid;
      out->attr.gid = it->second.st_gid;
      out->attr.size = it->second.st_size;
      out->attr.blksize = it->second.st_blksize;
      out->attr.blocks = it->second.st_blocks;
      out->attr.atime = it->second.st_atime;
      out->attr.atime_nsec = it->second.st_atim.tv_nsec;
      out->attr.mtime = it->second.st_mtime;
      out->attr.mtime_nsec = it->second.st_mtim.tv_nsec;
      out->attr.ctime = it->second.st_ctime;
      out->attr.ctime_nsec = it->second.st_ctim.tv_nsec;
    }
    return Status::OK();
  }
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("inode batch output is null");
    }
    out->clear();
    for (const InodeID requested_ino : inode_ids) {
      SwordFsInode inode;
      auto status = GetInode(requested_ino, &inode);
      if (status.IsNotFound()) {
        out->emplace_back(std::nullopt);
        continue;
      }
      if (!status.ok()) {
        return status;
      }
      out->emplace_back(std::move(inode));
    }
    return Status::OK();
  }
  void SetAttr(InodeID ino, struct stat attr) {
    attrs[ino] = attr;
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkNod(InodeID, std::string_view, uint32_t, uint64_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag) override {
    return Status::OK();
  }
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override {
    auto it = attrs.find(ino);
    if (it == attrs.end()) {
      return Status::NotFound("inode not found");
    }
    if (swordfs::metadata::HasSetAttrField(fields, SetAttrField::kSize)) {
      it->second.st_size = static_cast<off_t>(attr.size);
    }
    if (out != nullptr) {
      auto status = GetInode(ino, out);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }
  Status SetInodeFlags(InodeID, swordfs::metadata::InodeFlag, SwordFsInode *) override {
    return Status::NotSupported("inode flags");
  }
  Status SetXAttr(InodeID, std::string_view, std::string_view, swordfs::metadata::XAttrSetMode) override {
    return Status::NotSupported("xattr");
  }
  Status GetXAttr(InodeID, std::string_view, std::string *) override {
    return Status::NotSupported("xattr");
  }
  Status ListXAttrs(InodeID, std::vector<std::string> *) override {
    return Status::NotSupported("xattr");
  }
  Status RemoveXAttr(InodeID, std::string_view) override {
    return Status::NotSupported("xattr");
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID ino, uint64_t *size = nullptr, InodeFlag *inode_flags = nullptr) override {
    if (open_entered_ != nullptr) {
      auto *entered = open_entered_;
      auto *release = open_release_;
      open_entered_ = nullptr;
      open_release_ = nullptr;
      entered->post();
      release->wait();
    }
    if (size != nullptr) {
      auto it = attrs.find(ino);
      *size = it == attrs.end() ? 0 : static_cast<uint64_t>(it->second.st_size);
    }
    if (inode_flags != nullptr) {
      *inode_flags = InodeFlag::kNone;
    }
    return open_status;
  }
  Status PrepareReclaim(InodeID ino) override {
    ++prepare_reclaim_calls;
    last_reclaim_ino = ino;
    return prepare_reclaim_status;
  }
  Status CompleteReclaim(InodeID ino) override {
    ++complete_reclaim_calls;
    last_reclaim_ino = ino;
    return reclaim_status;
  }
  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingDeletesBatch(size_t, const swordfs::metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }
  Status CompletePendingDelete(std::string_view) override {
    return Status::OK();
  }
  Status AllocateChunkRevision(swordfs::metadata::ChunkRevision *revision) override {
    if (revision == nullptr) {
      return Status::InvalidArgument("chunk revision output is null");
    }
    *revision = next_revision_++;
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status CommitChunk(InodeID, const std::optional<swordfs::metadata::SwordFsChunk> &,
                     const swordfs::metadata::SwordFsChunk &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, swordfs::metadata::ChunkIndex, swordfs::metadata::SwordFsChunk *) override {
    return Status::NotFound("no chunk");
  }
  Status Truncate(InodeID ino, uint64_t size) override {
    auto it = attrs.find(ino);
    if (it == attrs.end()) {
      return Status::NotFound("inode not found");
    }
    it->second.st_size = static_cast<off_t>(size);
    return Status::OK();
  }

  int prepare_reclaim_calls = 0;
  int complete_reclaim_calls = 0;
  int get_inode_calls = 0;
  InodeID last_reclaim_ino = 0;
  Status prepare_reclaim_status = Status::OK();
  Status reclaim_status = Status::OK();
  Status open_status = Status::OK();
  std::vector<swordfs::metadata::SwordFsChunk> chunks;
  std::unordered_map<InodeID, struct stat> attrs;

  void BlockNextOpen(folly::fibers::Baton *entered, folly::fibers::Baton *release) {
    open_entered_ = entered;
    open_release_ = release;
  }

 private:
  swordfs::metadata::ChunkRevision next_revision_ = 1;
  folly::fibers::Baton *open_entered_{nullptr};
  folly::fibers::Baton *open_release_{nullptr};
};

void ResetVolumeFromFiberForTest() {
  swordfs::test::RunInTestThreadFromFiber([] { swordfs::volume::VolumeImpl::Initialize(); });
}

}  // namespace

// ────────────────────────────────────────────────────────────────
// InodeHandle reclaim-fence guards.
// ────────────────────────────────────────────────────────────────

namespace {

// Install engines with an ino whose nlink is whatever the test wants.
// Returns raw pointers to both engines for assertions.
struct Engines {
  TrackingMetaEngine *meta;
  FakeDataEngine *data;
};

Engines InstallEnginesForInode(InodeID ino, nlink_t nlink, off_t size = 0) {
  auto meta_up = std::make_unique<swordfs::test::ConfiguredMetaEngine<TrackingMetaEngine>>();
  auto data_up = std::make_unique<FakeDataEngine>();
  auto *meta = meta_up.get();
  auto *data = data_up.get();

  struct stat attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.st_nlink = nlink;
  attr.st_size = size;
  meta->SetAttr(ino, attr);

  SwordFsVolume config;
  config.chunk_size = 4096;
  swordfs::test::RunInTestThreadFromFiber([&] {
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta_up), std::move(data_up), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  return {meta, data};
}

}  // namespace

FIBER_TEST_F(FileHandleTest, GetAttrReflectsUnflushedWriteSize) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/1);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  struct stat attr{};
  ASSERT_TRUE(VfsImpl::GetAttr(7, &attr).ok());
  EXPECT_EQ(attr.st_size, static_cast<off_t>(payload->length()));
  EXPECT_EQ(attr.st_nlink, 1U);

  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, GetAttrKeepsUnlinkedMetadataWhileOverlayingLiveSize) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)data;

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  struct stat unlinked{};
  unlinked.st_nlink = 0;
  meta->SetAttr(7, unlinked);

  struct stat attr{};
  ASSERT_TRUE(VfsImpl::GetAttr(7, &attr).ok());
  EXPECT_EQ(attr.st_nlink, 0U);
  EXPECT_EQ(attr.st_size, static_cast<off_t>(payload->length()));

  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, GetAttrDoesNotShrinkPersistedSizeForInPlaceWrite) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/1, /*size=*/128);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("x");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  struct stat attr{};
  ASSERT_TRUE(VfsImpl::GetAttr(7, &attr).ok());
  EXPECT_EQ(attr.st_size, 128);

  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, ReadUsesSizeCapturedByOpenWithoutMetadataRefetch) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1, /*size=*/300);
  (void)data;

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &handle).ok());
  EXPECT_EQ(meta->get_inode_calls, 0);

  auto out = folly::IOBuf::create(1024);
  ASSERT_TRUE(handle->Read(1024, 0, out.get()).ok());
  EXPECT_EQ(out->length(), 300U);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), std::string(300, '\0'));
  EXPECT_EQ(meta->get_inode_calls, 0);

  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, SuccessfulSetAttrShrinkReplacesEarlierLiveSize) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/1);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  SwordFsAttr requested{};
  requested.size = 5;
  auto inode_handle = InodeHandleManager::Instance().Get(7, false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->SetAttr(requested, SetAttrField::kSize, nullptr).ok());

  struct stat attr{};
  ASSERT_TRUE(VfsImpl::GetAttr(7, &attr).ok());
  EXPECT_EQ(attr.st_size, 5);

  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, NonSizeSetAttrReplyPreservesLiveSize) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/1);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  struct stat requested{};
  requested.st_mode = 0600;
  struct stat returned{};
  ASSERT_TRUE(VfsImpl::SetAttr(7, &requested, static_cast<int>(SetAttrField::kMode), std::nullopt, &returned).ok());
  EXPECT_EQ(returned.st_size, static_cast<off_t>(payload->length()));

  ASSERT_TRUE(handle->Release().ok());
}

FIBER_TEST_F(FileHandleTest, OpenTruncateReplacesEarlierLiveSize) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/1);

  std::shared_ptr<FileHandle> first;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &first).ok());
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(first->Write(*payload, 0).ok());

  std::shared_ptr<FileHandle> truncating;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR | O_TRUNC, &truncating).ok());

  struct stat attr{};
  ASSERT_TRUE(VfsImpl::GetAttr(7, &attr).ok());
  EXPECT_EQ(attr.st_size, 0);

  ASSERT_TRUE(truncating->Release().ok());
  ASSERT_TRUE(first->Release().ok());
}

FIBER_TEST_F(FileHandleTest, ReclaimFenceBlocksOpenUntilReleased) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/1);
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);

  ASSERT_TRUE(inode_handle->TryStartReclaim());
  std::shared_ptr<FileHandle> blocked;
  EXPECT_TRUE(FileHandle::Open(7, O_RDONLY, &blocked).IsNotFound());

  inode_handle->FinishReclaim();
  std::shared_ptr<FileHandle> reopened;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &reopened).ok());
  ASSERT_TRUE(reopened->Release().ok());
}

FIBER_TEST_F(FileHandleTest, ReclaimFenceIsExclusive) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/0);
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);

  ASSERT_TRUE(inode_handle->TryStartReclaim());
  EXPECT_FALSE(inode_handle->TryStartReclaim()) << "only one reclaim attempt may own the local fence";
  inode_handle->FinishReclaim();
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, ConcurrentOpenSucceedsWhileLastCloseFlushesLinkedInode) {
  // A normal linked inode must remain openable while its last descriptor is
  // closing. The closing descriptor is still a live reference until Flush
  // finishes; using the reclaim fence for this window would incorrectly turn
  // an ordinary concurrent Open into NotFound.
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)meta;

  std::shared_ptr<FileHandle> closing_handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &closing_handle).ok());
  auto payload = folly::IOBuf::copyBuffer("x");
  ASSERT_TRUE(closing_handle->Write(*payload, 0).ok());

  folly::fibers::Baton put_entered;
  folly::fibers::Baton put_release;
  folly::fibers::Baton closing_thread_done;
  data->BlockNextPut(&put_entered, &put_release);

  std::atomic<bool> close_ok{false};
  std::thread closing_thread([&] {
    swordfs::test::RunInTestFiber([&] { close_ok.store(closing_handle->Release().ok()); });
    closing_thread_done.post();
  });
  const bool put_was_entered = swordfs::test::WaitForBaton(put_entered);
  EXPECT_TRUE(put_was_entered) << "last close must reach the controlled Flush/Put blocker";
  if (!put_was_entered) {
    put_release.post();
    swordfs::test::WaitForBatonOrAbort(closing_thread_done, "FileHandle closing-thread timeout cleanup");
    closing_thread.join();
    return;
  }

  std::shared_ptr<FileHandle> reopened;
  const auto open_status = FileHandle::Open(7, O_RDONLY, &reopened);
  EXPECT_TRUE(open_status.ok()) << open_status.message();

  put_release.post();
  swordfs::test::WaitForBatonOrAbort(closing_thread_done, "FileHandle closing-thread completion after Put release");
  closing_thread.join();
  ASSERT_NE(reopened, nullptr);
  EXPECT_TRUE(close_ok.load());

  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_FALSE(inode_handle->TryStartReclaim());
  ASSERT_TRUE(reopened->Release().ok());
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, FailedLastCloseFlushStillReleasesDescriptorReference) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);

  std::shared_ptr<FileHandle> handle;
  ASSERT_TRUE(FileHandle::Open(7, O_RDWR, &handle).ok());
  auto payload = folly::IOBuf::copyBuffer("x");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  data->put_status = Status::IOError("injected flush failure");
  const auto close_status = handle->Release();
  EXPECT_EQ(close_status.ToErrno(), EIO);
  EXPECT_EQ(meta->prepare_reclaim_calls, 0) << "Close must never execute durable reclaim itself";

  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_TRUE(inode_handle->TryStartReclaim()) << "failed flush must not leak the local reclaim fence";
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, FailedOpenReleasesItsReferenceWithoutReclaimingLinkedInode) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)data;
  meta->open_status = Status::Permission("denied");

  // Hold the shared per-inode object explicitly. The manager registry owns
  // only a weak_ptr, so after a failed FileHandle::Open there is otherwise no
  // requirement that an expired registry entry remain observable.
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);

  std::shared_ptr<FileHandle> handle;
  const auto status = FileHandle::Open(7, O_RDONLY, &handle);
  EXPECT_EQ(status.ToErrno(), EACCES);
  EXPECT_EQ(handle, nullptr);

  EXPECT_EQ(meta->prepare_reclaim_calls, 0);
  ASSERT_TRUE(inode_handle->TryStartReclaim()) << "failed open must release its temporary descriptor reference";
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, FailedSecondOpenReleasesOnlyItsOwnReference) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/1);
  (void)data;

  std::shared_ptr<FileHandle> first;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &first).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_FALSE(inode_handle->TryStartReclaim());

  meta->open_status = Status::Permission("denied");
  std::shared_ptr<FileHandle> second;
  const auto status = FileHandle::Open(7, O_RDONLY, &second);
  EXPECT_EQ(status.ToErrno(), EACCES);
  EXPECT_EQ(second, nullptr);
  EXPECT_FALSE(inode_handle->TryStartReclaim()) << "failed open must preserve the first descriptor reference";
  EXPECT_EQ(meta->prepare_reclaim_calls, 0);

  ASSERT_TRUE(first->Release().ok());
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, OpenInProgressBlocksReclaimAndFailedOpenReleasesItsReference) {
  ResetVolumeFromFiberForTest();
  auto [meta, data] = InstallEnginesForInode(7, /*nlink=*/0);
  (void)data;

  folly::fibers::Baton open_entered;
  folly::fibers::Baton open_release;
  folly::fibers::Baton opening_thread_done;
  meta->open_status = Status::Permission("denied");
  meta->BlockNextOpen(&open_entered, &open_release);

  std::atomic<int> open_code{0};
  std::thread opening_thread([&] {
    swordfs::test::RunInTestFiber([&] {
      std::shared_ptr<FileHandle> handle;
      const auto status = FileHandle::Open(7, O_RDONLY, &handle);
      open_code.store(status.ToErrno());
    });
    opening_thread_done.post();
  });

  // The opening operation already owns a temporary descriptor reference while
  // parked in the metadata check, so background reclaim must defer.
  const bool open_was_entered = swordfs::test::WaitForBaton(open_entered);
  EXPECT_TRUE(open_was_entered) << "Open must reach the controlled metadata blocker";
  if (!open_was_entered) {
    open_release.post();
    swordfs::test::WaitForBatonOrAbort(opening_thread_done, "FileHandle opening-thread timeout cleanup");
    opening_thread.join();
    return;
  }
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/false);
  EXPECT_NE(inode_handle, nullptr);
  if (inode_handle == nullptr) {
    open_release.post();
    swordfs::test::WaitForBatonOrAbort(opening_thread_done, "FileHandle opening-thread missing-inode cleanup");
    opening_thread.join();
    return;
  }
  EXPECT_FALSE(inode_handle->TryStartReclaim());

  // When the metadata check fails, Open releases only its temporary reference;
  // the durable orphan remains entirely an OrphanReclaimer concern.
  open_release.post();
  swordfs::test::WaitForBatonOrAbort(opening_thread_done, "FileHandle opening-thread completion after Open release");
  opening_thread.join();
  EXPECT_EQ(open_code.load(), EACCES);
  ASSERT_TRUE(inode_handle->TryStartReclaim()) << "failed Open must not leak a descriptor reference";
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, SecondReclaimCannotAcquireAnOwnedFence) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/0);
  auto inode_handle = InodeHandleManager::Instance().Get(7, /*create_if_missing=*/true);
  ASSERT_NE(inode_handle, nullptr);
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  EXPECT_FALSE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

FIBER_TEST_F(FileHandleTest, ReclaimFenceRefusesWhileAnOpenHandleHoldsTheInode) {
  ResetVolumeFromFiberForTest();
  InstallEnginesForInode(7, /*nlink=*/0);

  // Open a file handle on this inode. The tracking engine lets
  // everything through, so this just installs a tracked fh.
  std::shared_ptr<FileHandle> fh;
  ASSERT_TRUE(FileHandle::Open(7, O_RDONLY, &fh).ok());
  auto inode_handle = InodeHandleManager::Instance().Get(7, false);
  ASSERT_NE(inode_handle, nullptr);

  EXPECT_FALSE(inode_handle->TryStartReclaim());

  // Close only releases the reference; the worker can acquire the fence on a
  // later pass.
  ASSERT_TRUE(fh->Release().ok());
  ASSERT_TRUE(inode_handle->TryStartReclaim());
  inode_handle->FinishReclaim();
}

}  // namespace
}  // namespace swordfs::vfs
