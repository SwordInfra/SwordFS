// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl.
// All VfsImpl methods return utils::Status (no FUSE dependency),
// so they can be tested directly without any FUSE infrastructure.

#include <dirent.h>
#include <fcntl.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>
#include <linux/fuse.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "fuse/Vfs.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/FiberRuntime.hpp"
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

namespace {

struct FuseReplyCapture {
  fuse_ctx context{};
  std::mutex mutex;
  std::condition_variable cv;
  bool replied = false;
  std::optional<int> error;
  std::optional<fuse_entry_param> entry;

  bool Wait() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&] { return replied; });
  }
};

FuseReplyCapture *CaptureFor(fuse_req_t req) {
  return reinterpret_cast<FuseReplyCapture *>(req);
}

}  // namespace

extern "C" const fuse_ctx *fuse_req_ctx(fuse_req_t req) {
  return &CaptureFor(req)->context;
}

extern "C" int fuse_reply_err(fuse_req_t req, int err) {
  auto *capture = CaptureFor(req);
  {
    std::lock_guard lock(capture->mutex);
    capture->error = err;
    capture->replied = true;
  }
  capture->cv.notify_one();
  return 0;
}

extern "C" int fuse_reply_entry(fuse_req_t req, const fuse_entry_param *entry) {
  auto *capture = CaptureFor(req);
  {
    std::lock_guard lock(capture->mutex);
    capture->entry = *entry;
    capture->replied = true;
  }
  capture->cv.notify_one();
  return 0;
}

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
  TestDirIterator(std::vector<swordfs::metadata::SwordFsEntry> entries, int fail_seek_call, Status seek_failure,
                  Status peek_status)
      : entries_(std::move(entries)),
        fail_seek_call_(fail_seek_call),
        seek_failure_(std::move(seek_failure)),
        peek_status_(std::move(peek_status)) {
  }

  Status Seek(uint64_t cookie) override {
    ++seek_calls_;
    if (fail_seek_call_ != 0 && seek_calls_ == fail_seek_call_) {
      return seek_failure_;
    }
    position_ = cookie;
    return Status::OK();
  }

  Status Peek(swordfs::metadata::SwordFsEntry *entry, uint64_t *next_cookie) override {
    if (!peek_status_.ok()) {
      return peek_status_;
    }
    if (position_ >= entries_.size()) {
      return Status::EndOfDirectory("directory end");
    }
    *entry = entries_[position_];
    *next_cookie = position_ + 1;
    return Status::OK();
  }

  void Advance() override {
    ++position_;
  }

 private:
  std::vector<swordfs::metadata::SwordFsEntry> entries_;
  uint64_t position_ = 0;
  int seek_calls_ = 0;
  int fail_seek_call_ = 0;
  Status seek_failure_{Status::OK()};
  Status peek_status_{Status::OK()};
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
      out->attr.ino = 2;
    }
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    ++get_inode_calls_;
    if (out) {
      auto it = inodes_.find(ino);
      if (it != inodes_.end()) {
        *out = it->second;
      } else {
        *out = {};
        out->ino = ino;
        out->attr.ino = ino;
      }
    }
    return get_inode_status_;
  }
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override {
    ++get_inodes_calls_;
    max_get_inodes_batch_size_ = std::max(max_get_inodes_batch_size_, inode_ids.size());
    last_get_inodes_ids_ = inode_ids;
    if (out == nullptr) {
      return Status::InvalidArgument("inode batch output is null");
    }
    if (!get_inode_status_.ok()) {
      return get_inode_status_;
    }
    std::vector<std::optional<SwordFsInode>> result;
    result.reserve(inode_ids.size());
    for (const InodeID requested_ino : inode_ids) {
      auto it = inodes_.find(requested_ino);
      if (it == inodes_.end()) {
        result.emplace_back(std::nullopt);
      } else {
        result.emplace_back(it->second);
      }
    }
    if (get_inodes_captured_ != nullptr) {
      auto *captured = std::exchange(get_inodes_captured_, nullptr);
      auto *release = std::exchange(get_inodes_release_, nullptr);
      captured->post();
      release->wait();
    }
    *out = std::move(result);
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *out) override {
    if (out) {
      *out = {};
      out->ino = 100;
    }
    return call_status_;
  }
  Status MkNod(InodeID, std::string_view, uint32_t mode, uint64_t rdev, SwordFsInode *out) override {
    ++mknod_calls_;
    if (out) {
      *out = {};
      out->ino = 103;
      out->attr.ino = 103;
      out->attr.mode = mode;
      out->attr.rdev = rdev;
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
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override {
    auto it = inodes_.find(ino);
    if (it != inodes_.end() && swordfs::metadata::HasSetAttrField(fields, SetAttrField::kSize)) {
      it->second.attr.size = attr.size;
    }
    if (out != nullptr) {
      if (it != inodes_.end()) {
        *out = it->second;
      } else {
        *out = {};
        out->ino = ino;
        out->attr.ino = ino;
        if (swordfs::metadata::HasSetAttrField(fields, SetAttrField::kSize)) {
          out->attr.size = attr.size;
        }
      }
    }
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
      out->attr.ino = 2;
      out->attr.nlink = 2;
    }
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID, uint64_t *size = nullptr) override {
    ++open_calls_;
    if (size != nullptr) {
      *size = 0;
    }
    return open_status_;
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
      *iterator =
          std::make_shared<TestDirIterator>(dir_entries_, fail_dir_seek_call_, dir_seek_failure_, dir_peek_status_);
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

  void set_open_status(Status status) {
    open_status_ = std::move(status);
  }

  int open_calls() const {
    return open_calls_;
  }

  int mknod_calls() const {
    return mknod_calls_;
  }

  void set_get_inode_status(Status status) {
    get_inode_status_ = std::move(status);
  }

  int lookup_calls() const {
    return lookup_calls_;
  }

  int get_inodes_calls() const {
    return get_inodes_calls_;
  }

  int get_inode_calls() const {
    return get_inode_calls_;
  }

  size_t max_get_inodes_batch_size() const {
    return max_get_inodes_batch_size_;
  }

  const std::vector<InodeID> &last_get_inodes_ids() const {
    return last_get_inodes_ids_;
  }

  void set_inode(SwordFsInode inode) {
    inodes_[inode.ino] = std::move(inode);
  }

  void set_inode_for(InodeID requested_ino, SwordFsInode inode) {
    inodes_[requested_ino] = std::move(inode);
  }

  void set_dir_entries(std::vector<swordfs::metadata::SwordFsEntry> entries) {
    dir_entries_ = std::move(entries);
  }

  void BlockNextGetInodes(folly::fibers::Baton *captured, folly::fibers::Baton *release) {
    get_inodes_captured_ = captured;
    get_inodes_release_ = release;
  }

  void set_dir_seek_failure(int call, Status status) {
    fail_dir_seek_call_ = call;
    dir_seek_failure_ = std::move(status);
  }

  void set_dir_peek_status(Status status) {
    dir_peek_status_ = std::move(status);
  }

 private:
  Status call_status_{Status::OK()};
  Status open_status_{Status::OK()};
  Status get_inode_status_{Status::OK()};
  int lookup_calls_ = 0;
  int open_calls_ = 0;
  int mknod_calls_ = 0;
  int get_inode_calls_ = 0;
  int get_inodes_calls_ = 0;
  size_t max_get_inodes_batch_size_ = 0;
  std::vector<InodeID> last_get_inodes_ids_;
  folly::fibers::Baton *get_inodes_captured_ = nullptr;
  folly::fibers::Baton *get_inodes_release_ = nullptr;
  int fail_dir_seek_call_ = 0;
  Status dir_seek_failure_{Status::OK()};
  Status dir_peek_status_{Status::OK()};
  swordfs::metadata::ChunkRevision next_revision_ = 1;
  std::unordered_map<InodeID, SwordFsInode> inodes_;
  std::vector<swordfs::metadata::SwordFsEntry> dir_entries_{{".", DT_DIR, 1}};
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
    swordfs::utils::ShutdownFiberRuntime();
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

TEST_F(VfsImplIntegrationTest, FuseMknodRepliesWithAuthoritativeEntry) {
  FuseReplyCapture capture;
  capture.context.uid = 1000;
  capture.context.gid = 100;
  constexpr dev_t kDevice = static_cast<dev_t>(0x1234);

  swordfs::fuse::VfsHookFactory::SwordFsMknod(reinterpret_cast<fuse_req_t>(&capture), 1, "char-device", S_IFCHR | 0620,
                                              kDevice);

  ASSERT_TRUE(capture.Wait());
  ASSERT_FALSE(capture.error.has_value());
  ASSERT_TRUE(capture.entry.has_value());
  EXPECT_EQ(capture.entry->ino, 103U);
  EXPECT_EQ(capture.entry->attr.st_mode, static_cast<mode_t>(S_IFCHR | 0620));
  EXPECT_EQ(capture.entry->attr.st_rdev, kDevice);
  EXPECT_EQ(mock_meta_->mknod_calls(), 1);
}

TEST_F(VfsImplIntegrationTest, FuseMknodRepliesWithErrnoOnMetadataFailure) {
  mock_meta_->set_status(Status::AlreadyExists("duplicate"));
  FuseReplyCapture capture;

  swordfs::fuse::VfsHookFactory::SwordFsMknod(reinterpret_cast<fuse_req_t>(&capture), 1, "fifo", S_IFIFO | 0600, 0);

  ASSERT_TRUE(capture.Wait());
  ASSERT_TRUE(capture.error.has_value());
  EXPECT_EQ(*capture.error, EEXIST);
  EXPECT_FALSE(capture.entry.has_value());
  EXPECT_EQ(mock_meta_->mknod_calls(), 1);
}

TEST_F(VfsImplIntegrationTest, FuseStatxAcceptsNullFileInfo) {
  FuseReplyCapture capture;

  swordfs::fuse::VfsHookFactory::SwordFsStatx(reinterpret_cast<fuse_req_t>(&capture), 1, 0, STATX_BASIC_STATS, nullptr);

  ASSERT_TRUE(capture.Wait());
  ASSERT_TRUE(capture.error.has_value());
  EXPECT_EQ(*capture.error, ENOSYS);
}

TEST_F(VfsImplIntegrationTest, FuseStatxAcceptsNonNullFileInfo) {
  FuseReplyCapture capture;
  fuse_file_info file_info{};
  file_info.fh = 123;

  swordfs::fuse::VfsHookFactory::SwordFsStatx(reinterpret_cast<fuse_req_t>(&capture), 1, 0, STATX_BASIC_STATS,
                                              &file_info);

  ASSERT_TRUE(capture.Wait());
  ASSERT_TRUE(capture.error.has_value());
  EXPECT_EQ(*capture.error, ENOSYS);
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
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 0);
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirRejectsUnknownHandle) {
  std::string buf;
  EXPECT_EQ(VfsImpl::ReadDir(nullptr, 1, 4096, 0, 999999, &buf).code(), Status::kInvalidArgument);
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlus) {
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  auto status = VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusRejectsUnknownHandle) {
  std::string buf;
  EXPECT_EQ(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, 999999, &buf).code(), Status::kInvalidArgument);
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusAcceptsZeroSizeWithoutMetadataLookup) {
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf = "stale";
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 0, 0, fh, &buf).ok());
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 0);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusPropagatesInitialSeekFailure) {
  mock_meta_->set_dir_seek_failure(1, Status::IOError("seek failed"));
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());

  std::string buf;
  const auto status = VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf);
  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 0);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusPropagatesIteratorPeekFailure) {
  mock_meta_->set_dir_peek_status(Status::IOError("peek failed"));
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());

  std::string buf;
  const auto status = VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf);
  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 0);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusReturnsSuccessForEmptyDirectory) {
  mock_meta_->set_dir_entries({});
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());

  std::string buf = "stale";
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf).ok());
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 0);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusRejectsEntryThatCannotFitEmptyReply) {
  mock_meta_->set_dir_entries({{"entry", DT_REG, 2}});
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());

  std::string buf;
  const auto status = VfsImpl::ReadDirPlus(nullptr, 1, 1, 0, fh, &buf);
  EXPECT_EQ(status.code(), Status::kNoMemory);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 0);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusPropagatesBatchMetadataFailure) {
  mock_meta_->set_get_inode_status(Status::IOError("batch read failed"));
  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());

  std::string buf;
  const auto status = VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf);
  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusRejectsInodeIdentityMismatch) {
  constexpr InodeID kRequestedIno = 2;
  mock_meta_->set_dir_entries({{"file", DT_REG, kRequestedIno}});

  SwordFsInode wrong_inode;
  wrong_inode.ino = 3;
  wrong_inode.attr = SwordFsAttr(3, S_IFREG | 0644, 100, 200);
  mock_meta_->set_inode_for(kRequestedIno, wrong_inode);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  EXPECT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf).IsMalformed());
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusReturnsAuthoritativeInodeAttributes) {
  SwordFsInode inode;
  inode.ino = 1;
  inode.attr = SwordFsAttr(1, S_IFDIR | 0750, 123, 456);
  inode.attr.nlink = 3;
  inode.attr.size = 8192;
  inode.attr.blocks = 16;
  inode.attr.blksize = 4096;
  inode.attr.atime = 11;
  inode.attr.mtime = 22;
  inode.attr.ctime = 33;
  mock_meta_->set_inode(inode);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf).ok());
  ASSERT_GE(buf.size(), sizeof(fuse_direntplus));

  fuse_direntplus result{};
  std::memcpy(&result, buf.data(), sizeof(result));
  EXPECT_EQ(result.entry_out.nodeid, 1u);
  EXPECT_EQ(result.entry_out.attr.ino, 1u);
  EXPECT_EQ(result.entry_out.attr.mode, inode.attr.mode);
  EXPECT_EQ(result.entry_out.attr.nlink, inode.attr.nlink);
  EXPECT_EQ(result.entry_out.attr.uid, inode.attr.uid);
  EXPECT_EQ(result.entry_out.attr.gid, inode.attr.gid);
  EXPECT_EQ(result.entry_out.attr.size, inode.attr.size);
  EXPECT_EQ(result.entry_out.attr.blocks, inode.attr.blocks);
  EXPECT_EQ(result.entry_out.attr.atime, inode.attr.atime);
  EXPECT_EQ(result.entry_out.attr.mtime, inode.attr.mtime);
  EXPECT_EQ(result.entry_out.attr.ctime, inode.attr.ctime);
  EXPECT_EQ(result.entry_out.entry_valid, 0u);
  EXPECT_EQ(result.entry_out.attr_valid, 0u);

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusSkipsEntryWhoseInodeDisappeared) {
  mock_meta_->set_dir_entries({{"gone", DT_REG, 2}, {"present", DT_REG, 3}});
  SwordFsInode present;
  present.ino = 3;
  present.attr = SwordFsAttr(3, S_IFREG | 0644, 100, 200);
  present.attr.nlink = 1;
  mock_meta_->set_inode(present);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf).ok());
  ASSERT_GE(buf.size(), sizeof(fuse_direntplus));

  fuse_direntplus result{};
  std::memcpy(&result, buf.data(), sizeof(result));
  EXPECT_EQ(result.entry_out.nodeid, 3u);
  ASSERT_LE(FUSE_NAME_OFFSET_DIRENTPLUS + result.dirent.namelen, buf.size());
  EXPECT_EQ(std::string_view(buf.data() + FUSE_NAME_OFFSET_DIRENTPLUS, result.dirent.namelen), "present");
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusMayReturnEnumeratedNameAfterConcurrentRename) {
  constexpr InodeID kFileIno = 2;
  mock_meta_->set_dir_entries({{"before", DT_REG, kFileIno}});
  SwordFsInode inode;
  inode.ino = kFileIno;
  inode.attr = SwordFsAttr(kFileIno, S_IFREG | 0644, 100, 200);
  inode.attr.nlink = 1;
  inode.attr.size = 123;
  mock_meta_->set_inode(inode);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());

  // The opened iterator has already established its entry-enumeration view.
  // Model a concurrent rename in the namespace while leaving the same inode
  // alive. READDIRPLUS may return the previously enumerated name, but the attr
  // payload must remain bound to that inode rather than to the new pathname.
  mock_meta_->set_dir_entries({{"after", DT_REG, kFileIno}});

  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, fh, &buf).ok());
  ASSERT_GE(buf.size(), sizeof(fuse_direntplus));

  fuse_direntplus result{};
  std::memcpy(&result, buf.data(), sizeof(result));
  EXPECT_EQ(result.entry_out.nodeid, kFileIno);
  EXPECT_EQ(result.entry_out.attr.ino, kFileIno);
  EXPECT_EQ(result.entry_out.attr.size, inode.attr.size);
  ASSERT_LE(FUSE_NAME_OFFSET_DIRENTPLUS + result.dirent.namelen, buf.size());
  EXPECT_EQ(std::string_view(buf.data() + FUSE_NAME_OFFSET_DIRENTPLUS, result.dirent.namelen), "before");

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusComposesTrackedLiveSizeWithoutRefetchingPerEntry) {
  constexpr InodeID kFileIno = 42;
  const std::string payload = "live-before-flush";
  mock_meta_->set_dir_entries({{"file", DT_REG, kFileIno}});

  SwordFsInode persistent;
  persistent.ino = kFileIno;
  persistent.attr = SwordFsAttr(kFileIno, S_IFREG | 0644, 100, 200);
  persistent.attr.nlink = 1;
  persistent.attr.size = 0;
  mock_meta_->set_inode(persistent);

  struct fuse_file_info fi = {};
  ASSERT_TRUE(VfsImpl::Open(kFileIno, &fi).ok());
  auto data = folly::IOBuf::copyBuffer(payload);
  ASSERT_TRUE(VfsImpl::Write(kFileIno, *data, 0, fi.fh).ok());

  struct stat getattr_attr{};
  ASSERT_TRUE(VfsImpl::GetAttr(kFileIno, &getattr_attr).ok());
  ASSERT_EQ(getattr_attr.st_size, static_cast<off_t>(payload.size()));
  const int get_inode_calls_before_plus = mock_meta_->get_inode_calls();

  uint64_t dir_fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &dir_fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, dir_fh, &buf).ok());
  ASSERT_GE(buf.size(), sizeof(fuse_direntplus));

  fuse_direntplus result{};
  std::memcpy(&result, buf.data(), sizeof(result));
  EXPECT_EQ(result.entry_out.nodeid, kFileIno);
  EXPECT_EQ(result.entry_out.attr.size, static_cast<uint64_t>(getattr_attr.st_size));
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_EQ(mock_meta_->get_inode_calls(), get_inode_calls_before_plus);

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, dir_fh).ok());
  EXPECT_TRUE(VfsImpl::Release(kFileIno, fi.fh).ok());
}

TEST_F(VfsImplIntegrationTest, ReadDirPlusKeepsMetadataSnapshotAndLiveOverlaySynchronized) {
  constexpr InodeID kFileIno = 42;
  const std::string payload = "live-before-flush";
  mock_meta_->set_dir_entries({{"file", DT_REG, kFileIno}});

  SwordFsInode persistent;
  persistent.ino = kFileIno;
  persistent.attr = SwordFsAttr(kFileIno, S_IFREG | 0644, 100, 200);
  persistent.attr.nlink = 1;
  persistent.attr.size = 0;
  mock_meta_->set_inode(persistent);

  struct fuse_file_info fi = {};
  fi.flags = O_RDWR;
  uint64_t dir_fh = 0;
  swordfs::test::RunInTestFiber([&] {
    ASSERT_TRUE(VfsImpl::Open(kFileIno, &fi).ok());
    auto data = folly::IOBuf::copyBuffer(payload);
    ASSERT_TRUE(VfsImpl::Write(kFileIno, *data, 0, fi.fh).ok());
    ASSERT_TRUE(VfsImpl::OpenDir(1, &dir_fh).ok());
  });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton snapshot_captured;
  folly::fibers::Baton release_snapshot;
  folly::fibers::Baton setattr_started;
  folly::fibers::Baton setattr_done;
  folly::fibers::Baton readdir_done;
  mock_meta_->BlockNextGetInodes(&snapshot_captured, &release_snapshot);

  Status readdir_status;
  Status setattr_status;
  std::string buf;
  fm.addTask([&] {
    readdir_status = VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, dir_fh, &buf);
    readdir_done.post();
  });
  fm.addTask([&] {
    snapshot_captured.wait();
    setattr_started.post();
    struct stat requested{};
    requested.st_size = 1;
    setattr_status = VfsImpl::SetAttr(kFileIno, &requested, FUSE_SET_ATTR_SIZE, fi.fh, nullptr);
    setattr_done.post();
  });

  while (!snapshot_captured.try_wait() || !setattr_started.try_wait()) {
    evb.loopOnce();
  }
  // #212 requires authoritative metadata and the local size overlay to be one
  // coherent visible-attribute read. The size mutation must wait until this
  // READDIRPLUS snapshot has been composed rather than slipping between them.
  EXPECT_FALSE(setattr_done.try_wait());

  release_snapshot.post();
  while (!readdir_done.try_wait() || !setattr_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(readdir_status.ok()) << readdir_status.message();
  ASSERT_TRUE(setattr_status.ok()) << setattr_status.message();
  ASSERT_GE(buf.size(), sizeof(fuse_direntplus));

  fuse_direntplus result{};
  std::memcpy(&result, buf.data(), sizeof(result));
  EXPECT_EQ(result.entry_out.attr.size, payload.size());

  swordfs::test::RunInTestFiber([&] {
    struct stat after{};
    ASSERT_TRUE(VfsImpl::GetAttr(kFileIno, &after).ok());
    EXPECT_EQ(after.st_size, 1);
    EXPECT_TRUE(VfsImpl::ReleaseDir(1, dir_fh).ok());
    EXPECT_TRUE(VfsImpl::Release(kFileIno, fi.fh).ok());
  });
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusBatchesOnlyEntriesThatFitReplyBuffer) {
  mock_meta_->set_dir_entries({{"first", DT_REG, 2}, {"second", DT_REG, 3}});

  SwordFsInode first;
  first.ino = 2;
  first.attr = SwordFsAttr(2, S_IFREG | 0644, 100, 200);
  first.attr.nlink = 1;
  mock_meta_->set_inode(first);

  SwordFsInode second;
  second.ino = 3;
  second.attr = SwordFsAttr(3, S_IFREG | 0644, 100, 200);
  second.attr.nlink = 1;
  mock_meta_->set_inode(second);

  fuse_entry_param probe{};
  const size_t first_entry_size = fuse_add_direntry_plus(nullptr, nullptr, 0, "first", &probe, 1);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, first_entry_size, 0, fh, &buf).ok());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  ASSERT_EQ(mock_meta_->last_get_inodes_ids().size(), 1u);
  EXPECT_EQ(mock_meta_->last_get_inodes_ids()[0], 2u);

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusReusesReplySpaceReservedForMissingInode) {
  mock_meta_->set_dir_entries({{"gone", DT_REG, 2}, {"live", DT_REG, 3}});

  SwordFsInode live;
  live.ino = 3;
  live.attr = SwordFsAttr(3, S_IFREG | 0644, 100, 200);
  live.attr.nlink = 1;
  mock_meta_->set_inode(live);

  fuse_entry_param probe{};
  const size_t one_entry_size = fuse_add_direntry_plus(nullptr, nullptr, 0, "gone", &probe, 1);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, one_entry_size, 0, fh, &buf).ok());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 2);
  ASSERT_GE(buf.size(), sizeof(fuse_direntplus));

  fuse_direntplus result{};
  std::memcpy(&result, buf.data(), sizeof(result));
  EXPECT_EQ(result.entry_out.nodeid, 3u);
  ASSERT_LE(FUSE_NAME_OFFSET_DIRENTPLUS + result.dirent.namelen, buf.size());
  EXPECT_EQ(std::string_view(buf.data() + FUSE_NAME_OFFSET_DIRENTPLUS, result.dirent.namelen), "live");

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusPropagatesReseekFailureAfterMissingEntry) {
  mock_meta_->set_dir_entries({{"gone", DT_REG, 2}, {"live", DT_REG, 3}});
  SwordFsInode live;
  live.ino = 3;
  live.attr = SwordFsAttr(3, S_IFREG | 0644, 100, 200);
  mock_meta_->set_inode(live);
  mock_meta_->set_dir_seek_failure(2, Status::IOError("reseek failed"));

  fuse_entry_param probe{};
  const size_t one_entry_size = fuse_add_direntry_plus(nullptr, nullptr, 0, "gone", &probe, 1);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  const auto status = VfsImpl::ReadDirPlus(nullptr, 1, one_entry_size, 0, fh, &buf);
  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusRejectsOversizedEntryAfterSkippedMissingBatch) {
  const std::string large_name(200, 'x');
  mock_meta_->set_dir_entries({{"a", DT_REG, 2}, {large_name, DT_REG, 3}});

  fuse_entry_param probe{};
  const size_t first_entry_size = fuse_add_direntry_plus(nullptr, nullptr, 0, "a", &probe, 1);
  ASSERT_GT(fuse_add_direntry_plus(nullptr, nullptr, 0, large_name.c_str(), &probe, 2), first_entry_size);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  const auto status = VfsImpl::ReadDirPlus(nullptr, 1, first_entry_size, 0, fh, &buf);
  EXPECT_EQ(status.code(), Status::kNoMemory);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusStopsCleanlyWhenNextBatchCannotFitRemainingReply) {
  constexpr size_t kFirstBatchEntries = 128;
  std::vector<swordfs::metadata::SwordFsEntry> entries;
  entries.reserve(kFirstBatchEntries + 1);
  for (size_t i = 0; i <= kFirstBatchEntries; ++i) {
    const InodeID ino = 2000 + i;
    entries.push_back({"x", DT_REG, ino});
    SwordFsInode inode;
    inode.ino = ino;
    inode.attr = SwordFsAttr(ino, S_IFREG | 0644, 100, 200);
    mock_meta_->set_inode(std::move(inode));
  }
  mock_meta_->set_dir_entries(std::move(entries));

  fuse_entry_param probe{};
  const size_t entry_size = fuse_add_direntry_plus(nullptr, nullptr, 0, "x", &probe, 1);
  const size_t reply_size = kFirstBatchEntries * entry_size + entry_size / 2;

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, reply_size, 0, fh, &buf).ok());
  EXPECT_EQ(buf.size(), kFirstBatchEntries * entry_size);
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_EQ(mock_meta_->max_get_inodes_batch_size(), kFirstBatchEntries);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusStopsWhenReplyIsExactlyFullAfterOneBatch) {
  constexpr size_t kFirstBatchEntries = 128;
  std::vector<swordfs::metadata::SwordFsEntry> entries;
  entries.reserve(kFirstBatchEntries + 1);
  for (size_t i = 0; i <= kFirstBatchEntries; ++i) {
    const InodeID ino = 3000 + i;
    entries.push_back({"x", DT_REG, ino});
    SwordFsInode inode;
    inode.ino = ino;
    inode.attr = SwordFsAttr(ino, S_IFREG | 0644, 100, 200);
    mock_meta_->set_inode(std::move(inode));
  }
  mock_meta_->set_dir_entries(std::move(entries));

  fuse_entry_param probe{};
  const size_t entry_size = fuse_add_direntry_plus(nullptr, nullptr, 0, "x", &probe, 1);

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, kFirstBatchEntries * entry_size, 0, fh, &buf).ok());
  EXPECT_EQ(buf.size(), kFirstBatchEntries * entry_size);
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusHandlesTrackedHardlinksAndUntrackedNeighbor) {
  constexpr InodeID kUntrackedIno = 42;
  constexpr InodeID kTrackedIno = 43;
  mock_meta_->set_dir_entries(
      {{"untracked", DT_REG, kUntrackedIno}, {"hardlink-a", DT_REG, kTrackedIno}, {"hardlink-b", DT_REG, kTrackedIno}});

  for (const InodeID ino : {kUntrackedIno, kTrackedIno}) {
    SwordFsInode inode;
    inode.ino = ino;
    inode.attr = SwordFsAttr(ino, S_IFREG | 0644, 100, 200);
    inode.attr.nlink = ino == kTrackedIno ? 2 : 1;
    mock_meta_->set_inode(std::move(inode));
  }

  struct fuse_file_info tracked_fi = {};
  ASSERT_TRUE(VfsImpl::Open(kTrackedIno, &tracked_fi).ok());
  uint64_t dir_fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &dir_fh).ok());

  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 4096, 0, dir_fh, &buf).ok());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 1);
  EXPECT_EQ(mock_meta_->last_get_inodes_ids(), (std::vector<InodeID>{kUntrackedIno, kTrackedIno, kTrackedIno}));

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, dir_fh).ok());
  EXPECT_TRUE(VfsImpl::Release(kTrackedIno, tracked_fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, ReadDirPlusCapsAttributeBatchAt128Entries) {
  constexpr size_t kEntryCount = 257;
  std::vector<swordfs::metadata::SwordFsEntry> entries;
  entries.reserve(kEntryCount);
  for (size_t i = 0; i < kEntryCount; ++i) {
    const InodeID entry_ino = 1000 + i;
    entries.push_back({"entry-" + std::to_string(i), DT_REG, entry_ino});
    SwordFsInode inode;
    inode.ino = entry_ino;
    inode.attr = SwordFsAttr(entry_ino, S_IFREG | 0644, 100, 200);
    inode.attr.nlink = 1;
    mock_meta_->set_inode(std::move(inode));
  }
  mock_meta_->set_dir_entries(std::move(entries));

  uint64_t fh = 0;
  ASSERT_TRUE(VfsImpl::OpenDir(1, &fh).ok());
  std::string buf;
  ASSERT_TRUE(VfsImpl::ReadDirPlus(nullptr, 1, 1 << 20, 0, fh, &buf).ok());
  EXPECT_EQ(mock_meta_->get_inodes_calls(), 3);
  EXPECT_EQ(mock_meta_->max_get_inodes_batch_size(), 128u);

  EXPECT_TRUE(VfsImpl::ReleaseDir(1, fh).ok());
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
  mock_meta_->set_open_status(Status::Permission("denied"));

  struct fuse_file_info fi = {};
  auto status = VfsImpl::Open(42, &fi);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, CreateEstablishesHandleWithoutReopeningNewInode) {
  // CREATE is already one kernel-authorized create+open operation. The mode
  // of the inode just created must not be used to re-authorize that same open.
  mock_meta_->set_open_status(Status::Permission("existing-inode open must not run"));

  fuse_entry_param entry{};
  struct fuse_file_info fi = {};
  fi.flags = O_RDWR;
  auto status = VfsImpl::Create(1, "new-file", 0000, &entry, &fi);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(entry.ino, 100u);
  EXPECT_NE(fi.fh, 0u);
  EXPECT_EQ(mock_meta_->open_calls(), 0);
  ASSERT_TRUE(VfsImpl::Release(entry.ino, fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, MknodReturnsAuthoritativeEntry) {
  fuse_entry_param entry{};
  constexpr dev_t kDevice = static_cast<dev_t>(0x1234);
  auto status = VfsImpl::MkNod(1, "char-device", S_IFCHR | 0620, kDevice, &entry);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(entry.ino, 103U);
  EXPECT_EQ(entry.attr.st_ino, 103U);
  EXPECT_EQ(entry.attr.st_mode, static_cast<mode_t>(S_IFCHR | 0620));
  EXPECT_EQ(entry.attr.st_rdev, kDevice);
  EXPECT_EQ(entry.attr_timeout, 1.0);
  EXPECT_EQ(entry.entry_timeout, 1.0);
  EXPECT_EQ(mock_meta_->mknod_calls(), 1);
}

FIBER_TEST_F(VfsImplIntegrationTest, MknodRejectsDirectorySymlinkAndUnknownTypesBeforeMetadata) {
  fuse_entry_param entry{};
  EXPECT_EQ(VfsImpl::MkNod(1, "directory", S_IFDIR | 0700, 0, &entry).code(), Status::kInvalidArgument);
  EXPECT_EQ(VfsImpl::MkNod(1, "symlink", S_IFLNK | 0700, 0, &entry).code(), Status::kInvalidArgument);
  EXPECT_EQ(VfsImpl::MkNod(1, "unknown", 0700, 0, &entry).code(), Status::kInvalidArgument);
  EXPECT_EQ(mock_meta_->mknod_calls(), 0);
}

FIBER_TEST_F(VfsImplIntegrationTest, FtruncateRejectsReadOnlyFileHandle) {
  struct fuse_file_info fi = {};
  fi.flags = O_RDONLY;
  ASSERT_TRUE(VfsImpl::Open(42, &fi).ok());

  struct stat requested = {};
  requested.st_size = 17;
  auto status = VfsImpl::SetAttr(42, &requested, FUSE_SET_ATTR_SIZE, fi.fh, nullptr);

  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();
  ASSERT_TRUE(VfsImpl::Release(42, fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, FtruncateRejectsUnknownFileHandle) {
  struct stat requested = {};
  requested.st_size = 17;

  auto status = VfsImpl::SetAttr(42, &requested, FUSE_SET_ATTR_SIZE, 999999, nullptr);

  EXPECT_EQ(status.code(), Status::kInvalidArgument) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, FtruncateAcceptsWritableFileHandle) {
  struct fuse_file_info fi = {};
  fi.flags = O_WRONLY;
  ASSERT_TRUE(VfsImpl::Open(42, &fi).ok());

  struct stat requested = {};
  requested.st_size = 17;
  auto status = VfsImpl::SetAttr(42, &requested, FUSE_SET_ATTR_SIZE, fi.fh, nullptr);

  EXPECT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(VfsImpl::Release(42, fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, PathTruncateDoesNotRequireFileHandle) {
  struct stat requested = {};
  requested.st_size = 17;

  auto status = VfsImpl::SetAttr(42, &requested, FUSE_SET_ATTR_SIZE, std::nullopt, nullptr);

  EXPECT_TRUE(status.ok()) << status.message();
}

FIBER_TEST_F(VfsImplIntegrationTest, StatfsSuccess) {
  struct statvfs stbuf;
  auto status = VfsImpl::StatFs(1, &stbuf);
  EXPECT_TRUE(status.ok()) << status.message();
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

FIBER_TEST_F(VfsImplIntegrationTest, LookupEntryPreservesLiveSizeForOpenInode) {
  struct fuse_file_info fi{};
  ASSERT_TRUE(VfsImpl::Open(2, &fi).ok());
  auto handle = swordfs::vfs::HandleManager::Instance().FindAs<swordfs::vfs::FileHandle>(fi.fh);
  ASSERT_NE(handle, nullptr);
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  fuse_entry_param entry{};
  ASSERT_TRUE(VfsImpl::Lookup(1, "file", &entry).ok());
  EXPECT_EQ(entry.attr.st_size, static_cast<off_t>(payload->length()));

  ASSERT_TRUE(VfsImpl::Release(2, fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, LookupPropagatesTrackedInodeAttributeRefreshFailure) {
  struct fuse_file_info fi{};
  ASSERT_TRUE(VfsImpl::Open(2, &fi).ok());

  mock_meta_->set_get_inode_status(Status::IOError("injected lookup attribute refresh failure"));
  fuse_entry_param entry{};
  auto status = VfsImpl::Lookup(1, "file", &entry);

  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_EQ(status.message(), "injected lookup attribute refresh failure");

  mock_meta_->set_get_inode_status(Status::OK());
  ASSERT_TRUE(VfsImpl::Release(2, fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, LinkEntryPreservesLiveSizeForOpenInode) {
  struct fuse_file_info fi{};
  ASSERT_TRUE(VfsImpl::Open(2, &fi).ok());
  auto handle = swordfs::vfs::HandleManager::Instance().FindAs<swordfs::vfs::FileHandle>(fi.fh);
  ASSERT_NE(handle, nullptr);
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  fuse_entry_param entry{};
  ASSERT_TRUE(VfsImpl::Link(2, 1, "hardlink", &entry).ok());
  EXPECT_EQ(entry.attr.st_size, static_cast<off_t>(payload->length()));

  ASSERT_TRUE(VfsImpl::Release(2, fi.fh).ok());
}

FIBER_TEST_F(VfsImplIntegrationTest, LinkSuccessIsNotReversedByAttributeRefreshFailure) {
  struct fuse_file_info fi{};
  ASSERT_TRUE(VfsImpl::Open(2, &fi).ok());
  auto handle = swordfs::vfs::HandleManager::Instance().FindAs<swordfs::vfs::FileHandle>(fi.fh);
  ASSERT_NE(handle, nullptr);
  auto payload = folly::IOBuf::copyBuffer("Hello,_World!");
  ASSERT_TRUE(handle->Write(*payload, 0).ok());

  mock_meta_->set_get_inode_status(Status::IOError("injected post-link attribute refresh failure"));
  fuse_entry_param entry{};
  auto status = VfsImpl::Link(2, 1, "hardlink", &entry);

  EXPECT_TRUE(status.ok()) << "a committed hard link must not be reported as failed: " << status.message();
  EXPECT_EQ(entry.ino, 2U);
  EXPECT_EQ(entry.attr.st_nlink, 2U) << "failed enrichment must preserve the committed Link result";

  mock_meta_->set_get_inode_status(Status::OK());
  ASSERT_TRUE(VfsImpl::Release(2, fi.fh).ok());
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
