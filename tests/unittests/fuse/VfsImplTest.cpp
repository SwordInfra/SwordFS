// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl.
// All VfsImpl methods return utils::Status (no FUSE dependency),
// so they can be tested directly without any FUSE infrastructure.

#include <dirent.h>
#include <fcntl.h>
#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "fuse/Vfs.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "storage/IDataEngine.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/Reclaimer.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::vfs::VfsImpl;

// Minimal no-op data engine. The VfsImplIntegrationTest fixture must
// install one because every InodeHandle constructor asserts
// CHECK(data_engine != nullptr) — the production mount path always
// satisfies this (--bucket is required), but unit tests need to
// provide their own.
class NoopDataEngine : public swordfs::storage::IDataEngine {
 public:
  swordfs::utils::Status Initialize() override {
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Delete(std::string_view) override {
    return swordfs::utils::Status::OK();
  }
};

namespace folly {
class IOBuf;
}

// ────────────────────────────────────────────────────────────────
// Not-yet-implemented methods — all return NotSupported
// ────────────────────────────────────────────────────────────────

#define EXPECT_NOT_SUPPORTED(call)                                               \
  do {                                                                           \
    auto status = (call);                                                        \
    EXPECT_TRUE(status.IsNotSupported()) << #call << " => " << status.message(); \
  } while (0)

TEST(VfsImplTest, Mknod) {
  EXPECT_NOT_SUPPORTED(VfsImpl::MkNod(1, "test", 0644, 0));
}

TEST(VfsImplTest, Fsyncdir) {
  EXPECT_NOT_SUPPORTED(VfsImpl::FSyncDir(1, 0));
}

TEST(VfsImplTest, Setxattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::SetXAttr(1, "user.key", "val", 3, 0));
}

TEST(VfsImplTest, Getxattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::GetXAttr(1, "user.key", 256));
}

TEST(VfsImplTest, Listxattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::ListXAttr(1, 1024));
}

TEST(VfsImplTest, Removexattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::RemoveXAttr(1, "user.key"));
}

TEST(VfsImplTest, Ioctl) {
  EXPECT_NOT_SUPPORTED(VfsImpl::IoCtl(1, 0, nullptr, nullptr, 0, nullptr, 0, 0));
}

TEST(VfsImplTest, Flock) {
  EXPECT_NOT_SUPPORTED(VfsImpl::FLock(1, nullptr, 0));
}

TEST(VfsImplTest, Fallocate) {
  EXPECT_NOT_SUPPORTED(VfsImpl::FAllocate(1, 0, 0, 4096, nullptr));
}

TEST(VfsImplTest, Lseek) {
  EXPECT_NOT_SUPPORTED(VfsImpl::LSeek(1, 0, SEEK_SET, nullptr));
}

TEST(VfsImplTest, Tmpfile) {
  EXPECT_NOT_SUPPORTED(VfsImpl::TmpFile(1, 0644, nullptr));
}

TEST(VfsImplTest, Statx) {
  EXPECT_NOT_SUPPORTED(VfsImpl::StatX(1, 0, 0, nullptr));
}

// ────────────────────────────────────────────────────────────────
// RetrieveReply — also a stub
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, RetrieveReply) {
  EXPECT_NOT_SUPPORTED(VfsImpl::RetrieveReply(nullptr, nullptr, 1, 0, nullptr));
}

// ────────────────────────────────────────────────────────────────
// Forget / ForgetMulti — void methods, tested for no-crash
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, Forget) {
  // Needs a bound volume for meta_engine().  Without one this would
  // crash, so we can only test with a VolumeImpl that has a meta engine.
}

// ────────────────────────────────────────────────────────────────
// VfsImplIntegrationTest — methods requiring a mock IMetaEngine
// ────────────────────────────────────────────────────────────────
//
// These tests set up a VolumeImpl singleton with mock engines so that
// VfsImpl methods that delegate to meta_engine() can be exercised.

namespace {

class TestDirIterator final : public swordfs::metadata::DirIterator {
 public:
  Status Seek(uint64_t cookie) override {
    position_ = cookie;
    return Status::OK();
  }

  Status Peek(swordfs::metadata::SwordFsEntry *entry, uint64_t *next_cookie) override {
    if (position_ == 0) {
      *entry = {".", DT_DIR, 1};
      *next_cookie = 1;
      return Status::OK();
    }
    return Status::EndOfDirectory("directory end");
  }

  void Advance() override {
    ++position_;
  }

 private:
  uint64_t position_ = 0;
};

class MockMetaEngine : public swordfs::metadata::IMetaEngine {
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
  Status Lookup(InodeID, std::string_view, SwordFsInode *out) override {
    ++lookup_calls_;
    if (out) {
      *out = {};
      out->ino = 2;
    }
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = ino;
    }
    return call_status_;
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = 100;
    }
    return call_status_;
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = 101;
    }
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
  Status StatFs(SwordFsStatFs *stbuf) override {
    *stbuf = {};
    stbuf->name_max = 255;
    stbuf->fragment_size = 4096;
    stbuf->block_size = 4096;
    return call_status_;
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = 102;
    }
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = 2;
    }
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Access(InodeID, uint32_t) override {
    return call_status_;
  }
  Status Open(InodeID) override {
    return call_status_;
  }
  Status PrepareReclaim(InodeID, std::optional<swordfs::metadata::ReclaimWork> *work) override {
    work->reset();
    return Status::OK();
  }
  Status CompleteReclaim(InodeID) override {
    return Status::OK();
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
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *iterator) override {
    if (call_status_.ok() && iterator != nullptr) {
      *iterator = std::make_shared<TestDirIterator>();
    }
    return call_status_;
  }
  Status CommitChunk(InodeID, const std::optional<swordfs::metadata::SwordFsChunk> &,
                     const swordfs::metadata::SwordFsChunk &) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, ChunkIndex, SwordFsChunk *) override {
    return Status::NotFound("");
  }
  Status Truncate(InodeID, uint64_t) override {
    return Status::OK();
  }

  void set_status(Status s) {
    call_status_ = s;
  }

  int lookup_calls() const {
    return lookup_calls_;
  }

 private:
  Status call_status_{Status::OK()};
  int lookup_calls_ = 0;
  swordfs::metadata::ChunkRevision next_revision_ = 1;
};

class VfsImplIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::volume::VolumeImpl::Initialize();
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    auto mock = std::make_unique<MockMetaEngine>();
    mock_meta_ = mock.get();
    vol.set_meta_engine(std::move(mock));
    // InodeHandle's constructor asserts CHECK(data_engine != nullptr);
    // install a no-op so the test paths that go through Open succeed.
    vol.set_data_engine(std::make_unique<NoopDataEngine>());
  }

  void TearDown() override {
    // Reset singleton state for the next test.
    swordfs::volume::VolumeImpl::Initialize();
  }

  MockMetaEngine *mock_meta_ = nullptr;
};

}  // namespace

TEST(VfsHookFactoryTest, InitResetsInodeHandleRegistryBeforeReturning) {
  swordfs::volume::VolumeImpl::Initialize();
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::make_unique<MockMetaEngine>());
  vol.set_data_engine(std::make_unique<NoopDataEngine>());

  std::shared_ptr<swordfs::vfs::InodeHandle> stale_handle;
  swordfs::test::RunInTestFiber([&] {
    auto &manager = swordfs::vfs::InodeHandleManager::Instance();
    manager.Initialize();
    stale_handle = manager.Get(4242, /*create_if_missing=*/true);
    EXPECT_NE(stale_handle, nullptr);
    EXPECT_EQ(manager.Get(4242, /*create_if_missing=*/false), stale_handle);
  });

  struct fuse_conn_info conn{};
  swordfs::fuse::VfsHookFactory::SwordFsInit(nullptr, &conn);

  // SwordFsInit must not return while the mount-reset task is still queued:
  // a request dispatched immediately after init must never observe stale
  // per-inode state from a previous mount/test lifecycle.
  swordfs::test::RunInTestFiber(
      [&] { EXPECT_EQ(swordfs::vfs::InodeHandleManager::Instance().Get(4242, /*create_if_missing=*/false), nullptr); });

  swordfs::fuse::VfsHookFactory::SwordFsDestroy(nullptr);
  stale_handle.reset();
  swordfs::volume::VolumeImpl::Initialize();
}

FIBER_TEST_F(VfsImplIntegrationTest, UnlinkDoesNotPerformASeparateLookup) {
  auto status = VfsImpl::Unlink(1, "file");
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(mock_meta_->lookup_calls(), 0);
}

FIBER_TEST_F(VfsImplIntegrationTest, OpenDirSuccess) {
  uint64_t fh = 0;
  auto status = VfsImpl::OpenDir(1, &fh);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_NE(fh, 0u);
  // Clean up the directory handle.
  VfsImpl::ReleaseDir(1, fh);
}

FIBER_TEST_F(VfsImplIntegrationTest, OpenDirPermissionDenied) {
  mock_meta_->set_status(Status::Permission("denied"));

  uint64_t fh = 0;
  auto status = VfsImpl::OpenDir(1, &fh);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDir) {
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  auto status = VfsImpl::ReadDir(nullptr, 1, 4096, 0, fh, &buf);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlus) {
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  auto status = VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, ReleaseDirSuccess) {
  uint64_t fh = 0;
  auto status = VfsImpl::OpenDir(2, &fh);
  ASSERT_TRUE(status.ok());

  status = VfsImpl::ReleaseDir(2, fh);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, OpenSuccess) {
  struct fuse_file_info fi = {};
  auto status = VfsImpl::Open(42, &fi);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_NE(fi.fh, 0u);
  // Clean up — release the file handle via VfsImpl::Release.
  VfsImpl::Release(42, fi.fh);
}

FIBER_TEST_F(VfsImplIntegrationTest, OpenPermissionDenied) {
  mock_meta_->set_status(Status::Permission("denied"));

  struct fuse_file_info fi = {};
  auto status = VfsImpl::Open(42, &fi);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, StatfsSuccess) {
  struct statvfs stbuf;
  auto status = VfsImpl::StatFs(1, &stbuf);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, AccessSuccess) {
  auto status = VfsImpl::Access(1, R_OK);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, AccessDenied) {
  mock_meta_->set_status(Status::Permission("denied"));
  auto status = VfsImpl::Access(1, R_OK);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, GetattrSuccess) {
  struct stat attr;
  auto status = VfsImpl::GetAttr(1, &attr);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadlinkSuccess) {
  std::string target;
  auto status = VfsImpl::ReadLink(1, &target);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, SymlinkSuccess) {
  fuse_entry_param entry{};
  auto status = VfsImpl::Symlink("/target", 1, "link", &entry);
  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, LinkSuccess) {
  fuse_entry_param entry{};
  auto status = VfsImpl::Link(2, 1, "hardlink", &entry);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(entry.ino, 2u) << "Link should preserve ino";
}

// ────────────────────────────────────────────────────────────────
// #143: last-link cleanup at the VFS entry point
// ────────────────────────────────────────────────────────────────
// Unlink and rename-overwrite both end with the same decision: is this the
// inode's last name (nlink == 0), is a descriptor still holding it open, and
// did the cleanup that follows the committed metadata actually finish? The
// decision only exists against real nlink bookkeeping, so this fixture runs
// the real in-memory metadata engine and a data engine that can fail deletes.

namespace {

// Data engine that records every Delete and can fail selected keys.
class RecordingDataEngine : public swordfs::storage::IDataEngine {
 public:
  swordfs::utils::Status Initialize() override {
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    objects_[std::string(key)] = std::string(reinterpret_cast<const char *>(data->data()), data->length());
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return swordfs::utils::Status::OK();
  }
  swordfs::utils::Status Delete(std::string_view key) override {
    const std::string owned(key);
    delete_calls.push_back(owned);
    auto it = fail_keys.find(owned);
    if (it != fail_keys.end()) {
      return it->second;
    }
    objects_.erase(owned);
    return swordfs::utils::Status::OK();
  }

  void Seed(std::string key) {
    objects_[std::move(key)] = "seeded";
  }
  bool Contains(std::string_view key) const {
    return objects_.find(std::string(key)) != objects_.end();
  }

  std::vector<std::string> delete_calls;
  std::unordered_map<std::string, swordfs::utils::Status> fail_keys;

 private:
  std::unordered_map<std::string, std::string> objects_;
};

class VfsLastLinkCleanupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Volume lifecycle is control-plane work: initialize it on the gtest POSIX
    // thread, then reset the fiber-owned handle registry from a fiber.
    swordfs::volume::VolumeImpl::Initialize();
    swordfs::test::RunInTestFiber([&] { swordfs::vfs::InodeHandleManager::Instance().Initialize(); });

    auto meta = std::make_unique<swordfs::metadata::MemMetaImpl>();
    auto data = std::make_unique<RecordingDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_meta_engine(std::move(meta));
    vol.set_data_engine(std::move(data));
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] {
      // Release a descriptor a failed assertion left registered: its
      // InodeHandle would otherwise keep a reference into the engines
      // destroyed below.
      for (uint64_t fh : fhs_) {
        if (auto handle = swordfs::vfs::HandleManager::Instance().FindAs<swordfs::vfs::FileHandle>(fh)) {
          handle->Release();
        }
      }
    });
    swordfs::test::RunInTestFiber([&] { swordfs::vfs::InodeHandleManager::Instance().Initialize(); });
    swordfs::volume::VolumeImpl::Initialize();
  }

  // Create a regular file with one published chunk and seed its object.
  InodeID CreateChunkedFile(std::string_view name) {
    swordfs::metadata::SwordFsInode file;
    const auto created = meta_->Create(swordfs::metadata::kRootInodeId, name, 0644, &file);
    EXPECT_TRUE(created.ok()) << created.message();
    const swordfs::metadata::SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
    const auto committed = meta_->CommitChunk(file.ino, std::nullopt, chunk);
    EXPECT_TRUE(committed.ok()) << committed.message();
    data_->Seed(swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 1));
    return file.ino;
  }

  // Open a descriptor and remember it, so TearDown can release a leaked one.
  bool OpenDescriptor(InodeID ino, std::shared_ptr<swordfs::vfs::FileHandle> *out) {
    const auto status = swordfs::vfs::FileHandle::Open(ino, O_RDONLY, out);
    if (status.ok()) {
      fhs_.push_back((*out)->fh());
    }
    return status.ok();
  }

  std::vector<InodeID> OrphanCandidates() {
    std::vector<InodeID> out;
    const auto status = meta_->VisitOrphanCandidates([&out](InodeID ino) {
      out.push_back(ino);
      return swordfs::utils::Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    return out;
  }

  std::vector<InodeID> PendingReclaims() {
    std::vector<InodeID> out;
    const auto status = meta_->VisitPendingReclaims([&out](const swordfs::metadata::ReclaimWork &work) {
      out.push_back(work.ino);
      return swordfs::utils::Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    return out;
  }

  swordfs::metadata::MemMetaImpl *meta_ = nullptr;
  RecordingDataEngine *data_ = nullptr;
  std::vector<uint64_t> fhs_;
};

}  // namespace

FIBER_TEST_F(VfsLastLinkCleanupTest, UnlinkPublishesBackgroundCleanupAndOpenDescriptorDefersWorker) {
  const InodeID ino = CreateChunkedFile("f");
  const auto key = swordfs::chunk::FormatChunkObjectKey(ino, 0, 1);

  std::shared_ptr<swordfs::vfs::FileHandle> handle;
  ASSERT_TRUE(OpenDescriptor(ino, &handle));

  ASSERT_TRUE(VfsImpl::Unlink(swordfs::metadata::kRootInodeId, "f").ok());

  // The last name is gone, but the descriptor defers the cleanup: the object
  // and the inode must both survive while it can still be read.
  EXPECT_TRUE(data_->delete_calls.empty()) << "an open descriptor must defer the cleanup";
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));
  EXPECT_TRUE(PendingReclaims().empty());
  auto inode_handle = swordfs::vfs::InodeHandleManager::Instance().Get(ino, false);
  ASSERT_NE(inode_handle, nullptr);
  EXPECT_EQ(inode_handle->open_count(), 1U);

  // A worker pass while the descriptor is live must leave the durable orphan
  // untouched rather than crossing the metadata point of no return.
  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));

  // Last close only releases the local reference. GC remains background-owned.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));

  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_EQ(data_->delete_calls, (std::vector<std::string>{key}));
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(ino, nullptr).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(VfsLastLinkCleanupTest, UnlinkReturnsBeforeBackgroundCleanupAndRetrySurvivesFailure) {
  const InodeID ino = CreateChunkedFile("f");
  const auto key = swordfs::chunk::FormatChunkObjectKey(ino, 0, 1);
  data_->fail_keys[key] = swordfs::utils::Status::IOError("injected delete failure");

  // The unlink committed: the failed cleanup must not fail the syscall.
  const auto status = VfsImpl::Unlink(swordfs::metadata::kRootInodeId, "f");
  EXPECT_TRUE(status.ok()) << status.message();

  // Foreground unlink does not touch the object store or cross the metadata
  // point of no return. It only publishes durable orphan work and wakes the
  // worker.
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));
  EXPECT_TRUE(PendingReclaims().empty());

  // The worker prepares the inode, then the injected object-delete failure
  // leaves the frozen record durable for retry.
  EXPECT_EQ(swordfs::vfs::Reclaimer::Instance().Reconcile().code(), Status::kIOError);
  EXPECT_TRUE(meta_->GetInode(ino, nullptr).IsNotFound());
  EXPECT_EQ(PendingReclaims(), (std::vector<InodeID>{ino}));
  EXPECT_TRUE(OrphanCandidates().empty());

  // Reconciliation retries the idempotent delete and completes the reclaim.
  data_->fail_keys.clear();
  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(VfsLastLinkCleanupTest, UnlinkOfAHardlinkedNameDeletesNothing) {
  const InodeID ino = CreateChunkedFile("f");
  const auto key = swordfs::chunk::FormatChunkObjectKey(ino, 0, 1);

  swordfs::metadata::SwordFsInode linked;
  ASSERT_TRUE(meta_->Link(ino, swordfs::metadata::kRootInodeId, "g", &linked).ok());
  ASSERT_EQ(linked.attr.nlink, 2U);

  ASSERT_TRUE(VfsImpl::Unlink(swordfs::metadata::kRootInodeId, "f").ok());

  // Another name still references the inode: nothing may be reclaimed.
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
  swordfs::metadata::SwordFsInode remaining;
  ASSERT_TRUE(meta_->GetInode(ino, &remaining).ok());
  EXPECT_EQ(remaining.attr.nlink, 1U);

  // Removing the last name only publishes durable work; foreground unlink
  // must still issue zero object deletes.
  ASSERT_TRUE(VfsImpl::Unlink(swordfs::metadata::kRootInodeId, "g").ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{ino}));

  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(key));
  EXPECT_TRUE(meta_->GetInode(ino, nullptr).IsNotFound());
}

FIBER_TEST_F(VfsLastLinkCleanupTest, RenameOverwritePublishesBackgroundCleanupForReplacedInode) {
  constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
  const InodeID victim = CreateChunkedFile("victim");
  const InodeID moved = CreateChunkedFile("source");
  const auto victim_key = swordfs::chunk::FormatChunkObjectKey(victim, 0, 1);
  const auto moved_key = swordfs::chunk::FormatChunkObjectKey(moved, 0, 1);

  ASSERT_TRUE(VfsImpl::Rename(kRoot, "source", kRoot, "victim", 0).ok());

  // The rename commits immediately and only publishes durable orphan work for
  // the replaced inode; no object-store deletion belongs to the foreground
  // rename path.
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(data_->Contains(victim_key));
  EXPECT_TRUE(data_->Contains(moved_key));
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{victim}));

  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_EQ(data_->delete_calls, (std::vector<std::string>{victim_key}));
  EXPECT_FALSE(data_->Contains(victim_key));
  EXPECT_TRUE(data_->Contains(moved_key));
  EXPECT_TRUE(meta_->GetInode(victim, nullptr).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());

  swordfs::metadata::SwordFsInode renamed;
  ASSERT_TRUE(meta_->Lookup(swordfs::metadata::kRootInodeId, "victim", &renamed).ok());
  EXPECT_EQ(renamed.ino, moved);
}

FIBER_TEST_F(VfsLastLinkCleanupTest, RenameOverwritePublishesBackgroundCleanupAndOpenDescriptorDefersWorker) {
  constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
  const InodeID victim = CreateChunkedFile("victim");
  const InodeID moved = CreateChunkedFile("source");
  const auto victim_key = swordfs::chunk::FormatChunkObjectKey(victim, 0, 1);

  std::shared_ptr<swordfs::vfs::FileHandle> handle;
  ASSERT_TRUE(OpenDescriptor(victim, &handle));

  ASSERT_TRUE(VfsImpl::Rename(kRoot, "source", kRoot, "victim", 0).ok());

  // The renamed file is visible immediately; the replaced inode is only
  // orphaned, because its descriptor can still read it.
  swordfs::metadata::SwordFsInode renamed;
  ASSERT_TRUE(meta_->Lookup(swordfs::metadata::kRootInodeId, "victim", &renamed).ok());
  EXPECT_EQ(renamed.ino, moved);
  EXPECT_TRUE(data_->delete_calls.empty()) << "an open descriptor must defer the cleanup";
  EXPECT_TRUE(data_->Contains(victim_key));
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{victim}));

  // A worker pass while the victim descriptor is live must defer cleanup.
  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{victim}));

  // Last close releases the local reference only; the next worker pass owns
  // the actual reclaim.
  ASSERT_TRUE(handle->Release().ok());
  EXPECT_TRUE(data_->delete_calls.empty());
  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_EQ(data_->delete_calls, (std::vector<std::string>{victim_key}));
  EXPECT_FALSE(data_->Contains(victim_key));
  EXPECT_TRUE(meta_->GetInode(victim, nullptr).IsNotFound());
  EXPECT_TRUE(OrphanCandidates().empty());
}

FIBER_TEST_F(VfsLastLinkCleanupTest, RenameReturnsBeforeBackgroundCleanupAndRetrySurvivesFailure) {
  constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
  const InodeID victim = CreateChunkedFile("victim");
  const InodeID moved = CreateChunkedFile("source");
  const auto victim_key = swordfs::chunk::FormatChunkObjectKey(victim, 0, 1);
  data_->fail_keys[victim_key] = swordfs::utils::Status::IOError("injected delete failure");

  // The rename committed: the failed cleanup must not fail it.
  const auto status = VfsImpl::Rename(kRoot, "source", kRoot, "victim", 0);
  EXPECT_TRUE(status.ok()) << status.message();

  // The rename is visible and no foreground object delete was attempted.
  swordfs::metadata::SwordFsInode renamed;
  ASSERT_TRUE(meta_->Lookup(swordfs::metadata::kRootInodeId, "victim", &renamed).ok());
  EXPECT_EQ(renamed.ino, moved);
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(data_->Contains(victim_key));
  EXPECT_EQ(OrphanCandidates(), (std::vector<InodeID>{victim}));
  EXPECT_TRUE(PendingReclaims().empty());

  EXPECT_EQ(swordfs::vfs::Reclaimer::Instance().Reconcile().code(), Status::kIOError);
  EXPECT_EQ(PendingReclaims(), (std::vector<InodeID>{victim}));

  data_->fail_keys.clear();
  ASSERT_TRUE(swordfs::vfs::Reclaimer::Instance().Reconcile().ok());
  EXPECT_FALSE(data_->Contains(victim_key));
  EXPECT_TRUE(PendingReclaims().empty());
}

FIBER_TEST_F(VfsLastLinkCleanupTest, RenameWithoutAnOverwrittenInodeCleansUpNothing) {
  constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
  const InodeID source = CreateChunkedFile("source");
  const auto key = swordfs::chunk::FormatChunkObjectKey(source, 0, 1);

  ASSERT_TRUE(VfsImpl::Rename(kRoot, "source", kRoot, "target", 0).ok());

  // A plain rename replaces nothing: no inode is orphaned and no data may be
  // deleted.
  EXPECT_TRUE(data_->delete_calls.empty());
  EXPECT_TRUE(data_->Contains(key));
  EXPECT_TRUE(OrphanCandidates().empty());
  EXPECT_TRUE(PendingReclaims().empty());
  swordfs::metadata::SwordFsInode renamed;
  ASSERT_TRUE(meta_->Lookup(swordfs::metadata::kRootInodeId, "target", &renamed).ok());
  EXPECT_EQ(renamed.ino, source);
}
