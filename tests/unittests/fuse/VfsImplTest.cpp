// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl.
// All VfsImpl methods return utils::Status (no FUSE dependency),
// so they can be tested directly without any FUSE infrastructure.

#include <gtest/gtest.h>

#include <memory>

#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::vfs::VfsImpl;

// ────────────────────────────────────────────────────────────────
// Construction & binding
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, ConstructAndBind) {
  auto vfs = std::make_unique<VfsImpl>();
  auto vol = std::make_unique<swordfs::volume::VolumeImpl>();
  vfs->Init(std::move(vol));
  SUCCEED();
}

TEST(VfsImplTest, VolumeReturnsNonNullAfterInit) {
  auto vfs = std::make_unique<VfsImpl>();
  EXPECT_EQ(vfs->Volume(), nullptr);
  vfs->Init(std::make_unique<swordfs::volume::VolumeImpl>());
  EXPECT_NE(vfs->Volume(), nullptr);
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
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Readlink(1));
}

TEST(VfsImplTest, Mknod) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Mknod(1, "test", 0644, 0));
}

TEST(VfsImplTest, Symlink) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Symlink("/target", 1, "link"));
}

TEST(VfsImplTest, Link) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Link(2, 1, "hardlink"));
}

TEST(VfsImplTest, Fsyncdir) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Fsyncdir(1, 0));
}

TEST(VfsImplTest, Setxattr) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Setxattr(1, "user.key", "val", 3, 0));
}

TEST(VfsImplTest, Getxattr) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Getxattr(1, "user.key", 256));
}

TEST(VfsImplTest, Listxattr) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Listxattr(1, 1024));
}

TEST(VfsImplTest, Removexattr) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Removexattr(1, "user.key"));
}

TEST(VfsImplTest, Ioctl) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Ioctl(1, 0, nullptr, nullptr, 0, nullptr, 0, 0));
}

TEST(VfsImplTest, Flock) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Flock(1, nullptr, 0));
}

TEST(VfsImplTest, Fallocate) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Fallocate(1, 0, 0, 4096, nullptr));
}

TEST(VfsImplTest, Lseek) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Lseek(1, 0, SEEK_SET, nullptr));
}

TEST(VfsImplTest, Tmpfile) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Tmpfile(1, 0644, nullptr));
}

TEST(VfsImplTest, Statx) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.Statx(1, 0, 0, nullptr));
}

// ────────────────────────────────────────────────────────────────
// RetrieveReply — also a stub
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, RetrieveReply) {
  VfsImpl vfs;
  EXPECT_NOT_SUPPORTED(vfs.RetrieveReply(nullptr, nullptr, 1, 0, nullptr));
}

// ────────────────────────────────────────────────────────────────
// Readdir / Readdirplus — stubs that return OK
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, Readdir) {
  VfsImpl vfs;
  std::string buf;
  auto status = vfs.Readdir(1, 4096, 0, &buf);
  EXPECT_TRUE(status.ok());
}

TEST(VfsImplTest, Readdirplus) {
  VfsImpl vfs;
  std::string buf;
  auto status = vfs.Readdirplus(1, 4096, 0, &buf);
  EXPECT_TRUE(status.ok());
}

// ────────────────────────────────────────────────────────────────
// Forget / ForgetMulti — void methods, tested for no-crash
// ────────────────────────────────────────────────────────────────

TEST(VfsImplTest, Forget) {
  VfsImpl vfs;
  // Needs a bound volume for meta_engine().  Without one this would
  // crash, so we can only test with a VolumeImpl that has a meta engine.
  // For now we document this requires a proper fixture.
}

// ────────────────────────────────────────────────────────────────
// Methods requiring a meta engine (TODO: mock IMetaEngine)
// ────────────────────────────────────────────────────────────────
//
// These methods call vol_->meta_engine()->Xxx() and cannot be tested
// without a mock IMetaEngine:
//   Lookup, Getattr, Setattr, Mkdir, Unlink, Rmdir, Rename,
//   Open, Read, Write, Flush, Release, Fsync, Opendir,
//   Releasedir, Statfs, Access, Create, ForgetMulti
//
// They should be covered by integration tests that set up a real
// MemMetaStore-backed VolumeImpl.
