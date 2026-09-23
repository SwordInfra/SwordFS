// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// The public inode/chunk head and mechanism-private index participate in the
// same metadata operation. A deliberately rejecting participant verifies that
// its staged index writes and the public mutation are both left untouched.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/IChunkOverwriteStrategy.hpp"
#include "metadata/IChunkIndexTxn.hpp"
#include "metadata/mem/MemMetaStore.hpp"

namespace swordfs::metadata {
namespace {

std::string FragmentsHash(InodeID file_ino) {
  return "fragments:" + std::to_string(file_ino);
}

std::string FragmentField(const SwordFsChunk &head, unsigned part) {
  return std::to_string(head.index) + ":" + std::to_string(head.revision) + ":" + std::to_string(part);
}

class RecordingIndexParticipant final : public IChunkIndexParticipant {
 public:
  bool reject_publish = false;
  bool reject_truncate = false;
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

  utils::Status Truncate(IChunkIndexTxn &txn, InodeID file_ino,
                         const std::vector<ChunkIndexChange> &changes) const override {
    for (const auto &change : changes) {
      if (!change.current.has_value()) {
        for (unsigned part = 0; part != 2; ++part) {
          // Keep old generation records until physical cleanup confirms they
          // are unreachable; Redis EXEC can partially apply queued commands.
          auto status = txn.Put("retired", FragmentField(change.previous, part), "detached");
          if (!status.ok()) {
            return status;
          }
        }
      }
    }
    auto status = txn.Put("events", "truncate:" + std::to_string(file_ino), std::to_string(changes.size()));
    if (!status.ok()) {
      return status;
    }
    return reject_truncate ? utils::Status::IOError("reject private truncate") : utils::Status::OK();
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
};

class RecordingStrategy final : public chunk::IChunkOverwriteStrategy {
 public:
  RecordingIndexParticipant participant;

  std::string_view name() const override {
    return "recording_private_index";
  }
  uint32_t index_format_version() const override {
    return 1;
  }
  std::shared_ptr<chunk::IChunkSession> OpenSession(InodeID file_ino, ChunkIndex index) const override {
    return chunk::DefaultChunkOverwriteStrategy().OpenSession(file_ino, index);
  }
  const IChunkIndexParticipant &index_participant() const override {
    return participant;
  }

  utils::Status FreezePendingDelete(IChunkIndexTxn &, InodeID file_ino, const SwordFsChunk &head, uint64_t,
                                    PendingDelete *out) const override {
    *out = {.id = "candidate:" + std::to_string(file_ino) + ":" + std::to_string(head.index) + ":" +
                  std::to_string(head.revision),
            .index_format_version = 1,
            .payload = "opaque"};
    return utils::Status::OK();
  }
  utils::Status FreezeRejectedPublication(InodeID file_ino, const SwordFsChunk &replacement,
                                          const ChunkPublishIntent &intent, uint64_t,
                                          PendingDelete *out) const override {
    *out = {.id = "candidate:" + std::to_string(file_ino) + ":" + std::to_string(replacement.index) + ":" +
                  std::to_string(replacement.revision),
            .index_format_version = 1,
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
    *out = {.ino = file_ino, .index_format_version = 1, .payload = std::to_string(fragments.size())};
    return utils::Status::OK();
  }
  utils::Status DeletePending(const PendingDelete &, uint64_t, IMetaEngine *, storage::IDataEngine *,
                              bool *completed) const override {
    *completed = true;
    return utils::Status::OK();
  }
  utils::Status DeleteFrozen(const ReclaimWork &, uint64_t, IMetaEngine *, storage::IDataEngine *) const override {
    return utils::Status::OK();
  }
};

class ChunkIndexStrategyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    store_.BindChunkOverwriteStrategy(&strategy_);
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

  RecordingStrategy strategy_;
  MemMetaStore store_;
};

FIBER_TEST_F(ChunkIndexStrategyTest, PublishStagesMultiplePrivateRecordsWithPublicHead) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("file", &file).ok());
  const SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
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

  const SwordFsChunk replacement{.index = 0, .start_offset = 0, .revision = 2, .size = 96};
  const ChunkPublishIntent intent{.payload = "uploaded:r2"};
  strategy_.participant.reject_publish = true;
  EXPECT_EQ(Publish(file.ino, first, replacement, intent).code(), utils::Status::kIOError);
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

  strategy_.participant.reject_publish = false;
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

FIBER_TEST_F(ChunkIndexStrategyTest, TruncateAndSetAttrRejectWithoutPublicOrPrivateMutation) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("file", &file).ok());
  const SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 100};
  const SwordFsChunk second{.index = 1, .start_offset = 128, .revision = 2, .size = 70};
  ASSERT_TRUE(Publish(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(Publish(file.ino, std::nullopt, second).ok());

  strategy_.participant.reject_truncate = true;
  EXPECT_EQ(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 64); }).code(),
            utils::Status::kIOError);
  SwordFsInode inode;
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 198);
  SwordFsChunk head;
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head, first);
  ASSERT_TRUE(Find(file.ino, 1, &head).ok());
  EXPECT_EQ(head, second);
  EXPECT_EQ(Fragments(file.ino).size(), 4U);
  std::string marker;
  EXPECT_TRUE(Read("events", "truncate:" + std::to_string(file.ino), &marker).IsNotFound());

  SwordFsAttr requested = inode.attr;
  requested.size = 50;
  EXPECT_EQ(
      store_.Transact([&](MemMetaTxn &txn) { return txn.SetAttr(file.ino, requested, SetAttrField::kSize); }).code(),
      utils::Status::kIOError);
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 198);
  EXPECT_EQ(Fragments(file.ino).size(), 4U);

  strategy_.participant.reject_truncate = false;
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(file.ino, 64); }).ok());
  ASSERT_TRUE(Lookup(file.ino, &inode).ok());
  EXPECT_EQ(inode.attr.size, 64);
  ASSERT_TRUE(Find(file.ino, 0, &head).ok());
  EXPECT_EQ(head.size, 64);
  EXPECT_TRUE(Find(file.ino, 1, &head).IsNotFound());
  EXPECT_EQ(Fragments(file.ino).size(), 4U);
  ASSERT_TRUE(Read("events", "truncate:" + std::to_string(file.ino), &marker).ok());
  EXPECT_EQ(marker, "2");
  ASSERT_TRUE(Read("retired", FragmentField(second, 0), &marker).ok());
  EXPECT_EQ(marker, "detached");

  // Once the public head is gone, a later cleanup transaction may erase its
  // private records without affecting the surviving logical chunk.
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

FIBER_TEST_F(ChunkIndexStrategyTest, ReclaimFreezesPrivateIndexAndRejectsBeforeInodeRemoval) {
  SwordFsInode file;
  ASSERT_TRUE(AddFile("file", &file).ok());
  const SwordFsChunk first{.index = 0, .start_offset = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(Publish(file.ino, std::nullopt, first).ok());
  ASSERT_TRUE(store_.Transact([&](MemMetaTxn &txn) { return txn.Unlink(kRootInodeId, "file"); }).ok());

  strategy_.participant.reject_reclaim = true;
  std::optional<ReclaimWork> work;
  EXPECT_EQ(store_.Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(file.ino, &work); }).code(),
            utils::Status::kIOError);
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

  strategy_.participant.reject_reclaim = false;
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
