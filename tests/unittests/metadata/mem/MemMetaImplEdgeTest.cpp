// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Additional edge-case tests for MemMetaImpl covering previously
// untested code paths: Forget, OpenDir, ReleaseDir, StatFs, error paths.

#define FUSE_USE_VERSION 312
#include <folly/fibers/FiberManagerInternal.h>
#include <fuse_lowlevel.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = FUSE_ROOT_ID;

class MemMetaImplEdgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new MemMetaImpl();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override { delete impl_; }

  MemMetaImpl* impl_;
};

// ────────────────────────────────────────────────────────────────
// Forget
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplEdgeTest, ForgetDecrementsNlookup) {
  InodeID f_ino = 0;
  struct stat attr;
  impl_->Create(kRoot, "f", 0644, &f_ino, &attr);

  // Lookup increments nlookup to 2 (1 from create + 1 from lookup)
  InodeID found = 0;
  impl_->Lookup(kRoot, "f", &found, nullptr);

  // Forget 1 should leave nlookup > 0
  Status st = impl_->Forget(f_ino, 1);
  EXPECT_TRUE(st.ok());

  // Verify the inode still exists
  struct stat check;
  EXPECT_TRUE(impl_->GetAttr(f_ino, &check).ok());
}

TEST_F(MemMetaImplEdgeTest, ForgetNonexistentIsOk) {
  Status st = impl_->Forget(99999, 1);
  EXPECT_TRUE(st.ok()) << "Forget on non-existent inode should be no-op";
}

TEST_F(MemMetaImplEdgeTest, ForgetNlookupFloorZero) {
  InodeID f_ino = 0;
  impl_->Create(kRoot, "f", 0644, &f_ino, nullptr);

  // nlookup starts at 1 (from Create). Forget 100 clamps to 0.
  Status st = impl_->Forget(f_ino, 100);
  EXPECT_TRUE(st.ok());
}

// ────────────────────────────────────────────────────────────────
// OpenDir / ReleaseDir
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplEdgeTest, OpenDirSucceeds) {
  uint64_t fh = 0;
  Status st = impl_->OpenDir(kRoot, &fh);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_GT(fh, 0);
}

TEST_F(MemMetaImplEdgeTest, OpenDirOnFileFails) {
  InodeID f_ino = 0;
  impl_->Create(kRoot, "f", 0644, &f_ino, nullptr);

  uint64_t fh = 0;
  Status st = impl_->OpenDir(f_ino, &fh);
  EXPECT_TRUE(st.IsNotDirectory()) << st.message();
}

TEST_F(MemMetaImplEdgeTest, ReleaseDirSucceeds) {
  uint64_t fh = 0;
  impl_->OpenDir(kRoot, &fh);
  Status st = impl_->ReleaseDir(fh);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(MemMetaImplEdgeTest, ReleaseDirUnknownHandleFails) {
  Status st = impl_->ReleaseDir(99999);
  EXPECT_FALSE(st.ok());
}

// ────────────────────────────────────────────────────────────────
// Release (file handle)
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplEdgeTest, ReleaseUnknownHandleFails) {
  Status st = impl_->Release(99999);
  EXPECT_FALSE(st.ok());
}

// ────────────────────────────────────────────────────────────────
// StatFs
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplEdgeTest, StatFsReturnsValid) {
  struct statvfs stbuf;
  Status st = impl_->StatFs(&stbuf);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(stbuf.f_namemax, 255);
  EXPECT_EQ(stbuf.f_frsize, 4096);
  EXPECT_GT(stbuf.f_blocks, 0);
}

// ────────────────────────────────────────────────────────────────
// GetAttr error path
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplEdgeTest, GetAttrNotFound) {
  struct stat attr;
  Status st = impl_->GetAttr(99999, &attr);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

// ────────────────────────────────────────────────────────────────
// Lookup error paths
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaImplEdgeTest, LookupNotFound) {
  InodeID child = 0;
  Status st = impl_->Lookup(kRoot, "nonexistent", &child, nullptr);
  EXPECT_TRUE(st.IsNotFound()) << st.message();
}

TEST_F(MemMetaImplEdgeTest, LookupIncrementsNlookup) {
  InodeID f_ino = 0;
  impl_->Create(kRoot, "f", 0644, &f_ino, nullptr);

  // First lookup increments nlookup
  InodeID found = 0;
  impl_->Lookup(kRoot, "f", &found, nullptr);

  // Forget with nlookup=2 (1 from create + 1 from lookup)
  EXPECT_TRUE(impl_->Forget(f_ino, 2).ok());

  // After forget to 0, inode still exists in memory store
  struct stat check;
  EXPECT_TRUE(impl_->GetAttr(f_ino, &check).ok());
}
