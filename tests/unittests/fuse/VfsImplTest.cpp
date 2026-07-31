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
// Methods requiring a meta engine (TODO: mock IMetaEngine)
// ────────────────────────────────────────────────────────────────
//
// These methods call VolumeImpl::Instance().meta_engine()->Xxx() and cannot be tested
// without a mock IMetaEngine:
//   Lookup, Getattr, Setattr, Mkdir, Unlink, Rmdir, Rename,
//   Open, Read, Write, Flush, Release, Fsync, Opendir,
//   Releasedir, Statfs, Access, Create, ForgetMulti
//
// They should be covered by integration tests that set up a real
// MemMetaStore-backed VolumeImpl.
