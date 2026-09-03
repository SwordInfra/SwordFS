// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/FiberManagerInternal.h>
#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Context.hpp"
#include "vfs/GarbageCollector.hpp"

namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;
using swordfs::vfs::GarbageCollector;

class RecordingDataEngine final : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }

  swordfs::storage::DataEngineLimits Limits() const override {
    return {};
  }

  bool Head(std::string_view, size_t *) override {
    return false;
  }

  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return Status::NotSupported("unused");
  }

  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::NotSupported("unused");
  }

  Status Delete(std::string_view key) override {
    deleted_keys.emplace_back(key);
    if (fail_delete) {
      return Status::IOError("injected delete failure");
    }
    return Status::OK();
  }

  bool fail_delete = true;
  std::vector<std::string> deleted_keys;
};

TEST(GarbageCollectorTest, FailedDeleteRemainsDiscoverableAndRetries) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{};

  MemMetaImpl meta;

  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "file", 0644, &file).ok());
  const SwordFsChunk chunk{.index = 0, .start_offset = 0, .key = "chunk-object", .size = 10};
  ASSERT_TRUE(meta.AddChunk(file.ino, chunk).ok());
  ASSERT_TRUE(meta.Unlink(kRootInodeId, "file", nullptr).ok());

  RecordingDataEngine data;
  GarbageCollector collector(&meta, &data);
  EXPECT_FALSE(collector.Reclaim(file.ino).ok());
  EXPECT_TRUE(meta.GetInode(file.ino, &file).IsNotFound());

  std::vector<InodeID> pending;
  ASSERT_TRUE(meta.VisitPendingReclaims([&](InodeID ino) {
                    pending.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  ASSERT_EQ(pending, std::vector<InodeID>{file.ino});

  data.fail_delete = false;
  ASSERT_TRUE(collector.Reconcile().ok());
  ASSERT_EQ(data.deleted_keys, (std::vector<std::string>{"chunk-object", "chunk-object"}));

  pending.clear();
  ASSERT_TRUE(meta.VisitPendingReclaims([&](InodeID ino) {
                    pending.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_TRUE(pending.empty());
}

TEST(GarbageCollectorTest, RecoverPromotesCrashLeftOrphan) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{};

  MemMetaImpl meta;

  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "file", 0644, &file).ok());
  const SwordFsChunk chunk{.index = 0, .start_offset = 0, .key = "orphan-object", .size = 10};
  ASSERT_TRUE(meta.AddChunk(file.ino, chunk).ok());
  ASSERT_TRUE(meta.Unlink(kRootInodeId, "file", nullptr).ok());

  RecordingDataEngine data;
  data.fail_delete = false;
  GarbageCollector collector(&meta, &data);
  ASSERT_TRUE(collector.Recover().ok());

  EXPECT_TRUE(meta.GetInode(file.ino, &file).IsNotFound());
  ASSERT_EQ(data.deleted_keys, std::vector<std::string>{"orphan-object"});

  size_t pending_count = 0;
  ASSERT_TRUE(meta.VisitPendingReclaims([&](InodeID) {
                    ++pending_count;
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending_count, 0U);
}

}  // namespace
