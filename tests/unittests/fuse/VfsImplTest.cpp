// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl.
// All VfsImpl methods return utils::Status (no FUSE dependency),
// so they can be tested directly without any FUSE infrastructure.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "metadata/IMetaEngine.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::InodeID;
using swordfs::vfs::VfsImpl;

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

#define EXPECT_NOT_SUPPORTED(call)         \
  do {                                     \
    auto status = (call);                  \
    EXPECT_TRUE(status.IsNotSupported())   \
        << #call << " => " << status.message(); \
  } while (0)

TEST(VfsImplTest, Readlink) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Readlink(1));
}

TEST(VfsImplTest, Mknod) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Mknod(1, "test", 0644, 0));
}

TEST(VfsImplTest, Symlink) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Symlink("/target", 1, "link"));
}

TEST(VfsImplTest, Link) {
  EXPECT_NOT_SUPPORTED(VfsImpl::Link(2, 1, "hardlink"));
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
// Readdir / Readdirplus — stubs that return OK
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, Readdir) {
  std::string buf;
  auto status = VfsImpl::Readdir(1, 4096, 0, &buf);
  EXPECT_TRUE(status.ok());
}

TEST(VfsImplTest, Readdirplus) {
  std::string buf;
  auto status = VfsImpl::Readdirplus(1, 4096, 0, &buf);
  EXPECT_TRUE(status.ok());
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
  Status Lookup(InodeID, std::string_view, InodeID*,
                struct stat*) override {
    return Status::OK();
  }
  Status GetAttr(InodeID, struct stat* attr) override {
    std::memset(attr, 0, sizeof(*attr));
    return call_status_;
  }
  Status ReadDir(InodeID,
                 std::vector<swordfs::metadata::SwordFsEntry>*) override {
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, mode_t, InodeID* child_ino,
                struct stat* attr) override {
    *child_ino = 100;
    std::memset(attr, 0, sizeof(*attr));
    return call_status_;
  }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID*,
               struct stat*) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID,
                std::string_view, unsigned int) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const struct stat*, int,
                 struct stat*) override {
    return Status::OK();
  }
  Status StatFs(struct statvfs* stbuf) override {
    std::memset(stbuf, 0, sizeof(*stbuf));
    return call_status_;
  }
  Status Access(InodeID, int) override {
    return call_status_;
  }
  Status Open(InodeID) override { return call_status_; }
  Status OpenDir(InodeID) override { return call_status_; }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }
  Status AddChunk(InodeID, const swordfs::metadata::ChunkMeta&) override {
    return Status::OK();
  }
  Status FindChunk(InodeID, off_t, size_t, swordfs::metadata::ChunkMeta*) override {
    return Status::NotFound("");
  }

  void set_status(Status s) { call_status_ = s; }

 private:
  Status call_status_{Status::OK()};
};

class VfsImplIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::volume::VolumeImpl::Initialize();
    auto& vol = swordfs::volume::VolumeImpl::Instance();
    auto mock = std::make_unique<MockMetaEngine>();
    mock_meta_ = mock.get();
    vol.set_meta_engine(std::move(mock));
  }

  void TearDown() override {
    // Reset singleton state for the next test.
    swordfs::volume::VolumeImpl::Initialize();
  }

  MockMetaEngine* mock_meta_ = nullptr;
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
