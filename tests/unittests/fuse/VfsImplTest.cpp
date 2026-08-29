// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl.
// All VfsImpl methods return utils::Status (no FUSE dependency),
// so they can be tested directly without any FUSE infrastructure.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::RenameResult;
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
  swordfs::storage::DataEngineLimits Limits() const override {
    return {};
  }
  bool Head(std::string_view, size_t *) override {
    return false;
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
// Volume singleton access
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, VolumeReturnsNonNullAfterInit) {
  swordfs::volume::VolumeImpl::Initialize();
  EXPECT_NE(VfsImpl::Volume(), nullptr);
  swordfs::volume::VolumeImpl::Initialize();
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
  EXPECT_NOT_SUPPORTED(VfsImpl::Mknod(1, "test", 0644, 0));
}

TEST(VfsImplTest, Fsyncdir) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Fsyncdir(1, 0));
}

TEST(VfsImplTest, Setxattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Setxattr(1, "user.key", "val", 3, 0));
}

TEST(VfsImplTest, Getxattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Getxattr(1, "user.key", 256));
}

TEST(VfsImplTest, Listxattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Listxattr(1, 1024));
}

TEST(VfsImplTest, Removexattr) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Removexattr(1, "user.key"));
}

TEST(VfsImplTest, Ioctl) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Ioctl(1, 0, nullptr, nullptr, 0, nullptr, 0, 0));
}

TEST(VfsImplTest, Flock) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Flock(1, nullptr, 0));
}

TEST(VfsImplTest, Fallocate) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Fallocate(1, 0, 0, 4096, nullptr));
}

TEST(VfsImplTest, Lseek) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Lseek(1, 0, SEEK_SET, nullptr));
}

TEST(VfsImplTest, Tmpfile) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Tmpfile(1, 0644, nullptr));
}

TEST(VfsImplTest, Statx) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Statx(1, 0, 0, nullptr));
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
  Status ReadDir(InodeID, std::vector<swordfs::metadata::SwordFsEntry> *) override {
    return Status::OK();
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
  Status Unlink(InodeID, std::string_view, uint64_t *) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult *) override {
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
  Status ReclaimInode(InodeID) override {
    return call_status_;
  }
  Status ListChunks(InodeID, std::vector<swordfs::metadata::SwordFsChunk> *) override {
    return Status::OK();
  }
  Status OpenDir(InodeID) override {
    return call_status_;
  }
  Status AddChunk(InodeID, const swordfs::metadata::SwordFsChunk &) override {
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

 private:
  Status call_status_{Status::OK()};
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

TEST_F(VfsImplIntegrationTest, OpendirSuccess) {
  uint64_t fh = 0;
  auto status = VfsImpl::Opendir(1, &fh);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_NE(fh, 0u);
  // Clean up the directory handle.
  VfsImpl::Releasedir(1, fh);
}

TEST_F(VfsImplIntegrationTest, OpendirPermissionDenied) {
  mock_meta_->set_status(Status::Permission("denied"));

  uint64_t fh = 0;
  auto status = VfsImpl::Opendir(1, &fh);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

TEST_F(VfsImplIntegrationTest, Readdir) {
  std::string buf;
  auto status = VfsImpl::Readdir(nullptr, 1, 4096, 0, &buf);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, Readdirplus) {
  std::string buf;
  auto status = VfsImpl::Readdirplus(nullptr, 1, 4096, 0, &buf);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, ReleasedirSuccess) {
  uint64_t fh = 0;
  auto status = VfsImpl::Opendir(2, &fh);
  ASSERT_TRUE(status.ok());

  status = VfsImpl::Releasedir(2, fh);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, OpenSuccess) {
  struct fuse_file_info fi = {};
  auto status = VfsImpl::Open(42, &fi);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_NE(fi.fh, 0u);
  // Clean up — release the file handle via VfsImpl::Release.
  VfsImpl::Release(42, fi.fh);
}

TEST_F(VfsImplIntegrationTest, OpenPermissionDenied) {
  mock_meta_->set_status(Status::Permission("denied"));

  struct fuse_file_info fi = {};
  auto status = VfsImpl::Open(42, &fi);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

TEST_F(VfsImplIntegrationTest, StatfsSuccess) {
  struct statvfs stbuf;
  auto status = VfsImpl::Statfs(1, &stbuf);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, AccessSuccess) {
  auto status = VfsImpl::Access(1, R_OK);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, AccessDenied) {
  mock_meta_->set_status(Status::Permission("denied"));
  auto status = VfsImpl::Access(1, R_OK);
  EXPECT_TRUE(status.IsPermission()) << status.message();
}

TEST_F(VfsImplIntegrationTest, GetattrSuccess) {
  struct stat attr;
  auto status = VfsImpl::Getattr(1, &attr);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, ReadlinkSuccess) {
  std::string target;
  auto status = VfsImpl::Readlink(1, &target);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, SymlinkSuccess) {
  fuse_entry_param entry{};
  auto status = VfsImpl::Symlink("/target", 1, "link", &entry);
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST_F(VfsImplIntegrationTest, LinkSuccess) {
  fuse_entry_param entry{};
  auto status = VfsImpl::Link(2, 1, "hardlink", &entry);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(entry.ino, 2u) << "Link should preserve ino";
}
