// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Transitional bridge coverage only: the current public SwordFsChunk head and
// legacy raw bridge index still participate in one FileMetadata operation until
// #317/#318/#319 retire those callbacks. Typed ChunkMetadata is intentionally
// absent from these transactions.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/internal/ChunkMetadataBridge.hpp"
#include "metadata/IChunkIndexTxn.hpp"
#include "metadata/mem/MemCOWChunkMetadata.hpp"
#include "metadata/mem/MemMetaStore.hpp"

namespace swordfs::metadata {
namespace {

std::string FragmentsHash(InodeID file_ino) {
  return "fragments:" + std::to_string(file_ino);
}

std::string FragmentField(const SwordFsChunk &head, unsigned part) {
  return std::to_string(head.index) + ":" + std::to_string(head.revision) + ":" + std::to_string(part);
}

class RecordingBridge final : public chunk::internal::ChunkMetadataBridge {
 public:
  bool reject_publish = false;
  bool reject_pending_delete = false;
  bool reject_reclaim = false;

  utils::Status LoadPublished(IChunkIndexReader &reader, InodeID file_ino, const SwordFsChunk &head,
                              std::string *private_snapshot) const override {
    if (private_snapshot == nullptr) {
      return utils::Status::InvalidArgument("private snapshot output is null");
    }
    std::string first;
    auto status = reader.Read(FragmentsHash(file_ino), FragmentField(head, 0), &first);
    if (!status.ok()) {
      return status;
    }
    std::string second;
    status = reader.Read(FragmentsHash(file_ino), FragmentField(head, 1), &second);
    if (!status.ok()) {
      return status;
    }
    *private_snapshot = first + "," + second;
    return utils::Status::OK();
  }
  utils::Status Publish(IChunkIndexTxn &txn, InodeID file_ino, const std::optional<SwordFsChunk> &,
                        const SwordFsChunk &replacement, const ChunkPublishIntent &intent) const override {
    // Two independently addressable private records for one public head.
    const auto first = intent.payload.empty() ? "first" : intent.payload + "/first";
    const auto second = intent.payload.empty() ? "second" : intent.payload + "/second";
    auto status = txn.Put(FragmentsHash(file_ino), FragmentField(replacement, 0), first);
    if (!status.ok()) {
      return status;
    }
    status = txn.Put(FragmentsHash(file_ino), FragmentField(replacement, 1), second);
    if (!status.ok()) {
      return status;
    }
    return reject_publish ? utils::Status::IOError("reject private publication") : utils::Status::OK();
  }

  utils::Status PrepareReclaim(IChunkIndexTxn &txn, InodeID file_ino,
                               const std::vector<SwordFsChunk> &) const override {
    std::vector<std::pair<std::string, std::string>> fragments;
    auto status = txn.Scan(FragmentsHash(file_ino), &fragments);
    if (!status.ok()) {
      return status;
    }
    status = txn.Put("events", "reclaim:" + std::to_string(file_ino), std::to_string(fragments.size()));
    if (!status.ok()) {
      return status;
    }
    return reject_reclaim ? utils::Status::IOError("reject private reclaim") : utils::Status::OK();
  }

  utils::Status FreezePendingDelete(IChunkIndexTxn &, InodeID file_ino, const SwordFsChunk &head, uint64_t,
                                    PendingDelete *out) const override {
    if (reject_pending_delete) {
      return utils::Status::IOError("reject pending delete freeze");
    }
    *out = {.id = "candidate:" + std::to_string(file_ino) + ":" + std::to_string(head.index) + ":" +
                  std::to_string(head.revision),
            .payload = "opaque"};
    return utils::Status::OK();
  }
  utils::Status FreezeRejectedPublication(InodeID file_ino, const SwordFsChunk &replacement,
                                          const ChunkPublishIntent &intent, uint64_t,
                                          PendingDelete *out) const override {
    *out = {.id = "candidate:" + std::to_string(file_ino) + ":" + std::to_string(replacement.index) + ":" +
                  std::to_string(replacement.revision),
            .payload = intent.payload};
    return utils::Status::OK();
  }
  utils::Status FreezeReclaim(IChunkIndexTxn &txn, InodeID file_ino, const std::vector<SwordFsChunk> &, uint64_t,
                              ReclaimWork *out) const override {
    std::vector<std::pair<std::string, std::string>> fragments;
    auto status = txn.Scan(FragmentsHash(file_ino), &fragments);
    if (!status.ok()) {
      return status;
    }
    *out = {.ino = file_ino, .payload = std::to_string(fragments.size())};
    return utils::Status::OK();
  }
};

class ChunkPrivateMetadataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(store_.OpenChunkMetadata(ChunkType::kCow, &chunk_metadata_).ok());
    ASSERT_TRUE(store_.BindChunkMetadataBridge(&bridge_).ok());
    store_.SetChunkSize(128);
  }

  utils::Status AddFile(std::string_view name, SwordFsInode *out) {
    return store_.Transact([&](MemMetaTxn &txn) { return txn.AddEntry(kRootInodeId, name, S_IFREG | 0644, out); });
  }
  utils::Status Publish(InodeID file_ino, const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement,
                        const ChunkPublishIntent &intent = {}) {
    return store_.Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(file_ino, expected, replacement, intent); });
  }
  utils::Status Find(InodeID file_ino, ChunkIndex index, SwordFsChunk *out) {
    return store_.Transact([&](MemMetaTxn &txn) { return txn.FindChunk(file_ino, index, out); });
  }
  utils::Status Load(InodeID file_ino, ChunkIndex index, ChunkView *out) {
    return store_.Transact([&](MemMetaTxn &txn) { return txn.LoadChunkView(file_ino, index, out); });
  }
  utils::Status Lookup(InodeID file_ino, SwordFsInode *out) {
    return store_.Transact([&](MemMetaTxn &txn) { return txn.LookupInode(file_ino, out); });
  }
  utils::Status Read(std::string_view hash, std::string_view field, std::string *value) {
    return store_.Transact([&](MemMetaTxn &txn) { return txn.Read(hash, field, value); });
  }
  std::vector<std::pair<std::string, std::string>> Fragments(InodeID file_ino) {
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Scan(FragmentsHash(file_ino), &values); }).ok());
    return values;
  }

  RecordingBridge bridge_;
  ChunkMetadataPtr chunk_metadata_;
  MemMetaStore store_;
};

FIBER_TEST(ChunkMetadataBridgeTest, FactoryValidatesConstructionAndCOWSnapshotOutput) {
  std::unique_ptr<chunk::internal::ChunkMetadataBridge> bridge;
  EXPECT_EQ(chunk::internal::CreateChunkMetadataBridge(ChunkType::kCow, nullptr).ToErrno(), EINVAL);

  const auto unsupported_status = chunk::internal::CreateChunkMetadataBridge(ChunkType::kChunkSlice, &bridge);
  EXPECT_EQ(unsupported_status.ToErrno(), ENOSYS);
  EXPECT_EQ(unsupported_status.message(), "unsupported chunk metadata bridge type: chunk_slice");
  EXPECT_EQ(bridge, nullptr);

  ASSERT_TRUE(chunk::internal::CreateChunkMetadataBridge(ChunkType::kCow, &bridge).ok());
  ASSERT_NE(bridge, nullptr);

  MemMetaStore store;
  ChunkMetadataPtr chunk_metadata;
  ASSERT_TRUE(store.OpenChunkMetadata(ChunkType::kCow, &chunk_metadata).ok());
  const SwordFsChunk head{.index = 0, .revision = 1, .size = 64};
  const auto status =
      store.Transact([&](MemMetaTxn &txn) { return bridge->LoadPublished(txn, /*file_ino=*/42, head, nullptr); });
  EXPECT_EQ(status.ToErrno(), EINVAL);
}

FIBER_TEST(MemMetaTxnBindingTest, ChunkMutationsFailClosedUntilBridgeIsBound) {
  MemMetaStore store;
  store.SetChunkSize(128);

  SwordFsInode file;
  ASSERT_TRUE(
      store.Transact([&](MemMetaTxn &txn) { return txn.AddEntry(kRootInodeId, "unbound", S_IFREG | 0644, &file); })
          .ok());

  const SwordFsChunk replacement{.index = 0, .revision = 1, .size = 64};
  auto status = store.Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(file.ino, std::nullopt, replacement); });
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "chunk metadata bridge is not bound");

  ASSERT_TRUE(store.Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRootInodeId, "unbound"); }).ok());
  std::optional<ReclaimWork> work;
  status = store.Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(file.ino, &work); });
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "chunk metadata bridge is not bound");
  EXPECT_FALSE(work.has_value());
}

FIBER_TEST_F(ChunkPrivateMetadataTest, ChunkIDAllocationIsIndependentOfLegacyFileMetadataRevisionTransactions) {
  ASSERT_NE(chunk_metadata_, nullptr);
  EXPECT_EQ(chunk_metadata_->Type(), ChunkType::kCow);

  ChunkID first;
  ChunkID second;
  ASSERT_TRUE(chunk_metadata_->AllocateChunkID(&first).ok());
  EXPECT_EQ(first, ChunkID(1));
  ChunkRevision legacy_revision = 0;
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.AllocateChunkRevision(&legacy_revision); }).ok());
  EXPECT_EQ(legacy_revision, 1U);

  ASSERT_TRUE(chunk_metadata_->AllocateChunkID(&second).ok());
  EXPECT_EQ(second, ChunkID(2));
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.AllocateChunkRevision(&legacy_revision); }).ok());
  EXPECT_EQ(legacy_revision, 2U);
}

TEST(MemMetaStoreBindingTest, OpensChunkMetadataCapabilityAndRejectsNullBridge) {
  MemMetaStore store;
  EXPECT_EQ(store.OpenChunkMetadata(ChunkType::kCow, nullptr).ToErrno(), EINVAL);
  ChunkMetadataPtr chunk_metadata;
  ASSERT_TRUE(store.OpenChunkMetadata(ChunkType::kCow, &chunk_metadata).ok());
  ASSERT_NE(chunk_metadata, nullptr);
  EXPECT_EQ(chunk_metadata->Type(), ChunkType::kCow);
  EXPECT_NE(std::dynamic_pointer_cast<cow::COWChunkMetadata>(chunk_metadata), nullptr);
  ChunkMetadataPtr reopened;
  ASSERT_TRUE(store.OpenChunkMetadata(ChunkType::kCow, &reopened).ok());
  EXPECT_EQ(reopened, chunk_metadata);
  EXPECT_EQ(store.OpenChunkMetadata(ChunkType::kChunkSlice, &reopened).ToErrno(), ENOSYS);
  EXPECT_EQ(store.BindChunkMetadataBridge(nullptr).ToErrno(), EINVAL);
}

FIBER_TEST_F(ChunkPrivateMetadataTest, PublishStagesMultiplePrivateRecordsWithPublicHead) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("file", &file).ok());
  const SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(Publish(file.ino, std::nullopt, first).ok());

  SwordFsChunk head;
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, first);
  SwordFsInode inode;
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 64);
  EXPECT_EQ(Fragments(file.ino).size(), 2U);
  std::string value;
  ASSERT_TRUE(Read(FragmentsHash(file.ino), FragmentField(first, 0), &value).ok());
  EXPECT_EQ(value, "first");
  ASSERT_TRUE(Read(FragmentsHash(file.ino), FragmentField(first, 1), &value).ok());
  EXPECT_EQ(value, "second");
  ChunkView view;
  ASSERT_TRUE(Load(file.ino, 0, &view).ok());
  EXPECT_EQ(view.head, first);
  EXPECT_EQ(view.private_snapshot, "first,second");

  const SwordFsChunk replacement{.index = 0, .revision = 2, .size = 96};
  const ChunkPublishIntent intent{.payload = "uploaded:r2"};
  bridge_.reject_publish = true;
  EXPECT_EQ(Publish(file.ino, first, replacement, intent).ToErrno(), EIO);
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, first);
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 64);
  EXPECT_EQ(Fragments(file.ino).size(), 2U);
  EXPECT_TRUE(Read(FragmentsHash(file.ino), FragmentField(replacement, 0), &value).IsNotFound());
  std::vector<PendingDelete> pending;
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.ListPendingDeletes(pending); }).ok());
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending[0].id, "candidate:" + std::to_string(file.ino) + ":0:2");
  EXPECT_EQ(pending[0].payload, intent.payload);

  bridge_.reject_publish = false;
  ASSERT_TRUE(Publish(file.ino, first, replacement, intent).ok());
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, replacement);
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 96);
  EXPECT_EQ(Fragments(file.ino).size(), 4U);
  ASSERT_TRUE(Load(file.ino, 0, &view).ok());
  EXPECT_EQ(view.head, replacement);
  EXPECT_EQ(view.private_snapshot, "uploaded:r2/first,uploaded:r2/second");
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.ListPendingDeletes(pending); }).ok());
  ASSERT_EQ(pending.size(), 2U);
}

FIBER_TEST_F(ChunkPrivateMetadataTest, TruncateLeavesLegacyPrivateRecordsForIndependentCleanup) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("file", &file).ok());
  const SwordFsChunk first{.index = 0, .revision = 1, .size = 100};
  const SwordFsChunk second{.index = 1, .revision = 2, .size = 70};
  ASSERT_TRUE(Publish(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(Publish(file.ino, std::nullopt, second).ok());

  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 64); }).ok());
  SwordFsInode inode;
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 64);
  SwordFsChunk head;
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head.size, 64);
  EXPECT_TRUE(Find(file.ino, 1, &head).IsNotFound());
  EXPECT_EQ(Fragments(file.ino).size(), 4U);

  SwordFsAttr requested = inode.attr;
  requested.size = 50;
  ASSERT_TRUE(
      store_.Transact([&](MemMetaTxn &txn) { return txn.SetAttr(file.ino, requested, SetAttrField::kSize); }).ok());
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 50);
  EXPECT_EQ(Fragments(file.ino).size(), 4U);

  // The retired bridge no longer mutates mechanism-private state during
  // FileMetadata truncate. Detached state may leak until independent cleanup,
  // but it cannot become visible through the removed public descriptor.
  ASSERT_TRUE(store_
                  .Transact([&](MemMetaTxn &txn) {
                    auto status = txn.Erase(FragmentsHash(file.ino), FragmentField(second, 0));
                    if (!status.ok()) {
                      return status;
                    }
                    return txn.Erase(FragmentsHash(file.ino), FragmentField(second, 1));
                  })
                  .ok());
  EXPECT_EQ(Fragments(file.ino).size(), 2U);
}

FIBER_TEST_F(ChunkPrivateMetadataTest, LegacyTruncateExercisesCompleteSharedSizeLayoutContract) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("layout", &file).ok());
  const SwordFsChunk first{.index = 0, .revision = 11, .size = 100};
  const SwordFsChunk boundary{.index = 1, .revision = 12, .size = 40};
  const SwordFsChunk tail{.index = 2, .revision = 13, .size = 20};
  ASSERT_TRUE(Publish(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(Publish(file.ino, std::nullopt, boundary).ok());
  ASSERT_TRUE(Publish(file.ino, std::nullopt, tail).ok());

  // Interior shrink: chunk 0 is before the boundary, chunk 1 is already
  // shorter than the visible prefix, and chunk 2 is wholly detached.
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 192); }).ok());
  SwordFsChunk head;
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, first);
  ASSERT_TRUE(Find(file.ino, 1, &head).ok());
  EXPECT_EQ(head, boundary);
  EXPECT_TRUE(Find(file.ino, 2, &head).IsNotFound());

  // Exact-boundary shrink has no retained partial boundary.
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 128); }).ok());
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, first);
  EXPECT_TRUE(Find(file.ino, 1, &head).IsNotFound());

  // Invalid layout input is propagated rather than mutating FileMetadata.
  store_.SetChunkSize(0);
  EXPECT_EQ(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 64); }).ToErrno(), EINVAL);
  SwordFsInode inode;
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 128U);

  // A cleanup-freeze failure also leaves the public descriptor attached.
  store_.SetChunkSize(128);
  bridge_.reject_pending_delete = true;
  EXPECT_EQ(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 0); }).ToErrno(), EIO);
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, first);
}

FIBER_TEST_F(ChunkPrivateMetadataTest, ReclaimFreezesPrivateIndexAndRejectsBeforeInodeRemoval) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("file", &file).ok());
  const SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(Publish(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRootInodeId, "file"); }).ok());

  bridge_.reject_reclaim = true;
  std::optional<ReclaimWork> work;
  EXPECT_EQ(store_.Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(file.ino, &work); }).ToErrno(), EIO);
  EXPECT_FALSE(work.has_value());
  SwordFsInode inode;
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.nlink, 0);
  EXPECT_EQ(Fragments(file.ino).size(), 2U);
  std::string marker;
  EXPECT_TRUE(Read("events", "reclaim:" + std::to_string(file.ino), &marker).IsNotFound());
  std::vector<InodeID> orphans;
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.ListOrphanCandidates(&orphans); }).ok());
  EXPECT_EQ(orphans, std::vector<InodeID>{file.ino});

  bridge_.reject_reclaim = false;
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(file.ino, &work); }).ok());
  ASSERT_TRUE(work.has_value());
  EXPECT_EQ(work->ino, file.ino);
  EXPECT_EQ(work->payload, "2");
  EXPECT_TRUE(Lookup(file.ino, &inode).IsNotFound());
  ASSERT_TRUE(Read("events", "reclaim:" + std::to_string(file.ino), &marker).ok());
  EXPECT_EQ(marker, "2");
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.ListOrphanCandidates(&orphans); }).ok());
  EXPECT_TRUE(orphans.empty());
}

}  // namespace
}  // namespace swordfs::metadata
