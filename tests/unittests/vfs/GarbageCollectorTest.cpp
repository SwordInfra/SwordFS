// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/fibers/FiberManagerInternal.h>
#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
    if (fail_delete || (!fail_key.empty() && key == fail_key)) {
      return Status::IOError("injected delete failure");
    }
    return Status::OK();
  }

  bool fail_delete = true;
  std::string fail_key;
  std::vector<std::string> deleted_keys;
};

class ScriptedMetaEngine final : public MemMetaImpl {
 public:
  Status ReclaimInode(InodeID ino) override {
    prepared.push_back(ino);
    if (ino == fail_prepare_ino) {
      return Status::IOError("injected prepare failure");
    }
    return Status::OK();
  }

  Status VisitOrphanedInodes(const swordfs::metadata::InodeVisitorFn &visitor) override {
    if (fail_orphan_scan) {
      return Status::IOError("injected orphan scan failure");
    }
    for (InodeID ino : orphaned) {
      auto status = visitor(ino);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }

  Status VisitPendingReclaims(const swordfs::metadata::InodeVisitorFn &visitor) override {
    if (fail_pending_scan) {
      return Status::IOError("injected pending scan failure");
    }
    for (InodeID ino : pending) {
      auto status = visitor(ino);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }

  Status VisitReclaimChunks(InodeID ino, const swordfs::metadata::ChunkVisitorFn &visitor) override {
    visited_reclaims.push_back(ino);
    if (ino == fail_chunk_scan_ino) {
      return Status::IOError("injected chunk scan failure");
    }
    for (const auto &[owner, chunk] : reclaim_chunks) {
      if (owner == ino) {
        auto status = visitor(chunk);
        if (!status.ok()) {
          return status;
        }
      }
    }
    return Status::OK();
  }

  Status CompleteReclaim(InodeID ino) override {
    completed.push_back(ino);
    if (ino == fail_complete_ino) {
      return Status::IOError("injected completion failure");
    }
    return Status::OK();
  }

  InodeID fail_prepare_ino = 0;
  InodeID fail_chunk_scan_ino = 0;
  InodeID fail_complete_ino = 0;
  bool fail_orphan_scan = false;
  bool fail_pending_scan = false;
  std::vector<InodeID> orphaned;
  std::vector<InodeID> pending;
  std::vector<std::pair<InodeID, SwordFsChunk>> reclaim_chunks;
  std::vector<InodeID> prepared;
  std::vector<InodeID> visited_reclaims;
  std::vector<InodeID> completed;
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

TEST(GarbageCollectorTest, ReclaimStopsWhenPrepareFails) {
  ScriptedMetaEngine meta;
  meta.fail_prepare_ino = 7;
  RecordingDataEngine data;
  data.fail_delete = false;

  GarbageCollector collector(&meta, &data);
  const auto status = collector.Reclaim(7);

  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_EQ(meta.prepared, std::vector<InodeID>{7});
  EXPECT_TRUE(meta.visited_reclaims.empty());
  EXPECT_TRUE(meta.completed.empty());
  EXPECT_TRUE(data.deleted_keys.empty());
}

TEST(GarbageCollectorTest, ReconcileContinuesAfterFailureAndCompletesIndependentJobs) {
  ScriptedMetaEngine meta;
  meta.pending = {1, 2, 3};
  meta.reclaim_chunks = {
      {1, SwordFsChunk{.index = 0, .key = "bad-object"}},
      {3, SwordFsChunk{.index = 0, .key = "good-object"}},
  };
  RecordingDataEngine data;
  data.fail_delete = false;
  data.fail_key = "bad-object";

  GarbageCollector collector(&meta, &data);
  const auto status = collector.Reconcile();

  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_EQ(meta.visited_reclaims, (std::vector<InodeID>{1, 2, 3}));
  EXPECT_EQ(meta.completed, (std::vector<InodeID>{2, 3}));
  EXPECT_EQ(data.deleted_keys, (std::vector<std::string>{"bad-object", "good-object"}));
}

TEST(GarbageCollectorTest, ReconcilePropagatesMetadataFailures) {
  RecordingDataEngine data;
  data.fail_delete = false;

  ScriptedMetaEngine pending_scan_failure;
  pending_scan_failure.fail_pending_scan = true;
  GarbageCollector pending_collector(&pending_scan_failure, &data);
  EXPECT_EQ(pending_collector.Reconcile().code(), Status::kIOError);
  EXPECT_TRUE(pending_scan_failure.visited_reclaims.empty());

  ScriptedMetaEngine chunk_scan_failure;
  chunk_scan_failure.pending = {11};
  chunk_scan_failure.fail_chunk_scan_ino = 11;
  GarbageCollector chunk_collector(&chunk_scan_failure, &data);
  EXPECT_EQ(chunk_collector.Reconcile().code(), Status::kIOError);
  EXPECT_TRUE(chunk_scan_failure.completed.empty());

  ScriptedMetaEngine completion_failure;
  completion_failure.pending = {12};
  completion_failure.fail_complete_ino = 12;
  GarbageCollector completion_collector(&completion_failure, &data);
  EXPECT_EQ(completion_collector.Reconcile().code(), Status::kIOError);
  EXPECT_EQ(completion_failure.completed, std::vector<InodeID>{12});
}

TEST(GarbageCollectorTest, RecoverContinuesAfterPrepareFailureAndReturnsFirstError) {
  ScriptedMetaEngine meta;
  meta.orphaned = {21, 22};
  meta.pending = {22};
  meta.fail_prepare_ino = 21;
  RecordingDataEngine data;
  data.fail_delete = false;

  GarbageCollector collector(&meta, &data);
  const auto status = collector.Recover();

  EXPECT_EQ(status.code(), Status::kIOError);
  EXPECT_EQ(meta.prepared, (std::vector<InodeID>{21, 22}));
  EXPECT_EQ(meta.completed, std::vector<InodeID>{22});
}

TEST(GarbageCollectorTest, RecoverPropagatesEnumerationFailures) {
  RecordingDataEngine data;
  data.fail_delete = false;

  ScriptedMetaEngine orphan_scan_failure;
  orphan_scan_failure.fail_orphan_scan = true;
  GarbageCollector orphan_collector(&orphan_scan_failure, &data);
  EXPECT_EQ(orphan_collector.Recover().code(), Status::kIOError);
  EXPECT_TRUE(orphan_scan_failure.prepared.empty());

  ScriptedMetaEngine pending_scan_failure;
  pending_scan_failure.orphaned = {31};
  pending_scan_failure.fail_pending_scan = true;
  GarbageCollector pending_collector(&pending_scan_failure, &data);
  EXPECT_EQ(pending_collector.Recover().code(), Status::kIOError);
  EXPECT_EQ(pending_scan_failure.prepared, std::vector<InodeID>{31});
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

TEST(GarbageCollectorTest, MemoryMetadataVisitorsValidateAndPropagateCallbacks) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  MemMetaImpl meta;

  EXPECT_EQ(meta.VisitOrphanedInodes({}).code(), Status::kInvalidArgument);
  EXPECT_EQ(meta.VisitPendingReclaims({}).code(), Status::kInvalidArgument);
  EXPECT_EQ(meta.VisitReclaimChunks(1, {}).code(), Status::kInvalidArgument);

  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(meta.AddChunk(file.ino, SwordFsChunk{.index = 0, .key = "object"}).ok());
  ASSERT_TRUE(meta.Unlink(kRootInodeId, "file", nullptr).ok());

  EXPECT_EQ(meta.VisitOrphanedInodes([](InodeID) { return Status::IOError("stop orphan scan"); }).code(),
            Status::kIOError);
  ASSERT_TRUE(meta.ReclaimInode(file.ino).ok());
  EXPECT_EQ(meta.VisitPendingReclaims([](InodeID) { return Status::IOError("stop pending scan"); }).code(),
            Status::kIOError);
  EXPECT_EQ(
      meta.VisitReclaimChunks(file.ino, [](const SwordFsChunk &) { return Status::IOError("stop chunk scan"); }).code(),
      Status::kIOError);

  ASSERT_TRUE(meta.CompleteReclaim(file.ino).ok());
  EXPECT_TRUE(meta.CompleteReclaim(file.ino).ok());
}

TEST(GarbageCollectorTest, MemoryMetadataLinkCancelsOrphanCandidate) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  MemMetaImpl meta;

  SwordFsInode file;
  ASSERT_TRUE(meta.Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(meta.Unlink(kRootInodeId, "file", nullptr).ok());
  ASSERT_TRUE(meta.Link(file.ino, kRootInodeId, "linked-again", &file).ok());

  size_t orphan_count = 0;
  ASSERT_TRUE(meta.VisitOrphanedInodes([&](InodeID) {
                    ++orphan_count;
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(orphan_count, 0U);
  ASSERT_TRUE(meta.ReclaimInode(file.ino).ok());
  EXPECT_TRUE(meta.GetInode(file.ino, &file).ok());
}

TEST(GarbageCollectorTest, MemoryMetadataRenameOverwritePublishesOrphan) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  MemMetaImpl meta;

  SwordFsInode source;
  SwordFsInode victim;
  ASSERT_TRUE(meta.Create(kRootInodeId, "source", 0644, &source).ok());
  ASSERT_TRUE(meta.Create(kRootInodeId, "victim", 0644, &victim).ok());

  swordfs::metadata::RenameResult result;
  ASSERT_TRUE(
      meta.Rename(kRootInodeId, "source", kRootInodeId, "victim", swordfs::metadata::RenameFlag::kNone, &result).ok());
  EXPECT_EQ(result.overwritten_ino, victim.ino);

  std::vector<InodeID> orphaned;
  ASSERT_TRUE(meta.VisitOrphanedInodes([&](InodeID ino) {
                    orphaned.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(orphaned, std::vector<InodeID>{victim.ino});
}

}  // namespace
