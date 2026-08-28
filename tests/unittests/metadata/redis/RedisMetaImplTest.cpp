// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>

#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"

namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::RedisMetaConfig;
using swordfs::metadata::RedisMetaImpl;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsVolume;
using swordfs::metadata::kRootInodeId;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

bool LoadConfig(RedisMetaConfig *config) {
  const char *url = std::getenv("SWORDFS_REDIS_TEST_URL");
  if (url == nullptr) return false;
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
    static std::atomic<uint64_t> sequence{0};
    volume_name_ = "redis-meta-test-" + std::to_string(++sequence);
    impl_ = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    ASSERT_TRUE(impl_->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    volume.meta_url = "redis://test";
    ASSERT_TRUE(impl_->FormatVolume(volume).ok());
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }

  std::unique_ptr<RedisMetaImpl> impl_;
  RedisMetaConfig config_;
  std::string volume_name_;
};

TEST_F(RedisMetaImplTest, RenameDirectoryOverEmptyDirectoryUpdatesSameParentNlink) {
  SwordFsInode src;
  SwordFsInode dst;
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "src", 0777, &src).ok());
  ASSERT_TRUE(impl_->MkDir(kRootInodeId, "dst", 0777, &dst).ok());

  SwordFsInode root;
  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  ASSERT_EQ(root.attr.nlink, 4U);

  ASSERT_TRUE(impl_->Rename(kRootInodeId, "src", kRootInodeId, "dst", swordfs::metadata::RenameFlag::kNone, nullptr).ok());

  ASSERT_TRUE(impl_->GetInode(kRootInodeId, &root).ok());
  EXPECT_EQ(root.attr.nlink, 3U);
}

TEST_F(RedisMetaImplTest, SetAttrShrinkRemovesAndClampsChunks) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());

  SwordFsChunk chunk0;
  chunk0.index = 0;
  chunk0.start_offset = 0;
  chunk0.size = 4096;
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk0).ok());

  SwordFsChunk chunk1;
  chunk1.index = 1;
  chunk1.start_offset = 4096;
  chunk1.size = 4096;
  ASSERT_TRUE(impl_->AddChunk(file.ino, chunk1).ok());

  SwordFsAttr requested = file.attr;
  requested.size = 100;
  ASSERT_TRUE(impl_->SetAttr(file.ino, requested, SetAttrField::kSize, nullptr).ok());

  SwordFsChunk actual;
  ASSERT_TRUE(impl_->FindChunk(file.ino, 0, &actual).ok());
  EXPECT_EQ(actual.size, 100U);
  EXPECT_TRUE(impl_->FindChunk(file.ino, 1, &actual).IsNotFound());
}

}  // namespace
