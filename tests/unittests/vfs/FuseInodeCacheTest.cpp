// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "FiberTest.hpp"
#include "metadata/Types.hpp"
#include "vfs/FuseInodeCache.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::SwordFsInode;

class FuseInodeCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::test::RunInTestFiber([&] { FuseInodeCache::Instance().Initialize(); });
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] { FuseInodeCache::Instance().Initialize(); });
  }
};

TEST_F(FuseInodeCacheTest, PartialForgetRetainsSnapshotUntilFinalReference) {
  constexpr InodeID kIno = 42;
  SwordFsInode inode;
  inode.ino = kIno;
  inode.attr.ino = kIno;
  inode.attr.nlink = 1;
  inode.attr.uid = 1000;

  swordfs::test::RunInTestFiber([&] {
    auto &cache = FuseInodeCache::Instance();
    cache.RetainLookup(inode);
    cache.RetainLookup(inode);
    cache.Forget(kIno, 1);

    SwordFsInode detached;
    ASSERT_TRUE(cache.ResolveDetachedAfterNotFound(kIno, &detached));
    EXPECT_EQ(detached.attr.uid, 1000U);
    EXPECT_EQ(detached.attr.nlink, 0U);

    cache.Forget(kIno, 1);
    EXPECT_FALSE(cache.ResolveDetachedAfterNotFound(kIno, &detached));
  });
}

TEST_F(FuseInodeCacheTest, DetachedSnapshotIgnoresLaterLiveRefreshAndRetain) {
  constexpr InodeID kIno = 43;
  SwordFsInode original;
  original.ino = kIno;
  original.attr.ino = kIno;
  original.attr.nlink = 1;
  original.attr.uid = 1000;

  SwordFsInode newer = original;
  newer.attr.uid = 2000;

  swordfs::test::RunInTestFiber([&] {
    auto &cache = FuseInodeCache::Instance();
    cache.RetainLookup(original);

    SwordFsInode detached;
    ASSERT_TRUE(cache.ResolveDetachedAfterNotFound(kIno, &detached));
    ASSERT_EQ(detached.attr.uid, 1000U);

    cache.RefreshIfRetained(newer);
    cache.RetainLookup(newer);
    ASSERT_TRUE(cache.ResolveDetachedAfterNotFound(kIno, &detached));
    EXPECT_EQ(detached.attr.uid, 1000U);
    EXPECT_EQ(detached.attr.nlink, 0U);

    cache.Forget(kIno, 2);
  });
}

TEST_F(FuseInodeCacheTest, MissingAndZeroForgetAreHarmless) {
  constexpr InodeID kIno = 44;
  SwordFsInode inode;
  inode.ino = kIno;
  inode.attr.ino = kIno;
  inode.attr.nlink = 1;

  swordfs::test::RunInTestFiber([&] {
    auto &cache = FuseInodeCache::Instance();
    SwordFsInode out;
    EXPECT_FALSE(cache.ResolveDetachedAfterNotFound(kIno, &out));

    cache.RetainLookup(inode);
    cache.Forget(kIno, 0);
    EXPECT_TRUE(cache.ResolveDetachedAfterNotFound(kIno, &out));

    cache.Forget(kIno, 1);
  });
}

}  // namespace
}  // namespace swordfs::vfs
