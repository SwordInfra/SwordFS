// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileReadWriter — read, write, flush, and
// cross-file-handle sharing behaviour.

#include <fcntl.h>
#include <folly/ScopeGuard.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "FiberTest.hpp"
#include "TestWatchdog.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/Chunk.hpp"
#include "chunk/ChunkTestSupport.hpp"
#include "chunk/SliceChunkMetadataTestSupport.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/FileReadWriter.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::storage::IDataEngine;
using swordfs::test::RunInTestFiber;
using swordfs::utils::Status;
using swordfs::vfs::FileReadWriter;
using swordfs::vfs::InodeHandle;

constexpr size_t kMaxParallelFlushes = 2;
constexpr uint64_t kTestChunkSize = 1024;

// Exercise the production file-wide Flush scheduler against the Slice private
// index and the real conditional FileMetadata publication contract. The
// injected fiber yield represents Redis I/O between snapshot and commit.
class ContendedSliceMetaEngine : public swordfs::test::chunk_test::MissingMetaEngine {
 public:
  Status OpenChunkMetadata(swordfs::metadata::ChunkType type, swordfs::metadata::ChunkMetadataPtr *out) override {
    if (out == nullptr || type != swordfs::metadata::ChunkType::kChunkSlice) {
      return Status::InvalidArgument("expected Slice metadata");
    }
    *out = slice_metadata_;
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    swordfs::metadata::FileChunkSnapshot snapshot;
    auto status = MissingMetaEngine::ReadFileChunkSnapshot(ino, 0, &snapshot);
    if (status.ok()) {
      *out = snapshot.inode;
    }
    return status;
  }
  Status AttachPrepared(InodeID ino, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                        const swordfs::metadata::FileSizePrecondition &expected) override {
    return SimulateMetadataTransaction(
        /*attached=*/false, [&] { return MissingMetaEngine::AttachPrepared(ino, index, id, end, expected); });
  }
  Status FinalizeAttachedWrite(InodeID ino, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                               const swordfs::metadata::FileSizePrecondition &expected) override {
    return SimulateMetadataTransaction(
        /*attached=*/true, [&] { return MissingMetaEngine::FinalizeAttachedWrite(ino, index, id, end, expected); });
  }

  size_t peak_finalizations_ = 0;
  size_t peak_attached_finalizations_ = 0;

 private:
  template <typename Fn>
  Status SimulateMetadataTransaction(bool attached, Fn &&commit) {
    ++active_finalizations_;
    peak_finalizations_ = std::max(peak_finalizations_, active_finalizations_);
    if (attached) {
      ++active_attached_finalizations_;
      peak_attached_finalizations_ = std::max(peak_attached_finalizations_, active_attached_finalizations_);
    }
    const auto retire = folly::makeGuard([&] {
      --active_finalizations_;
      if (attached) {
        --active_attached_finalizations_;
      }
    });
    // Redis metadata transactions suspend fibers. Extend that window so a
    // second Chunk's publisher can enter before the first transaction returns.
    for (int i = 0; i < 12; ++i) {
      folly::fibers::yield();
    }
    return commit();
  }

 private:
  size_t active_finalizations_ = 0;
  size_t active_attached_finalizations_ = 0;
  swordfs::metadata::ChunkMetadataPtr slice_metadata_ =
      std::make_shared<swordfs::test::chunk_test::TestSliceChunkMetadata>();
};

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

static std::string Repeat(char c, size_t n) {
  return std::string(n, c);
}

static auto Buf(const std::string &s) {
  return *folly::IOBuf::copyBuffer(s.data(), s.size());
}

TEST(FileReadWriterSliceTest, ParallelChunkUploadMustNotStarveFileMetadataPublication) {
  // Twenty chunks share one inode. With uncontrolled concurrent finalization,
  // each snapshot is invalidated by a different Chunk's legitimate EOF grow,
  // exhausting Slice's finite publication attempts despite no external writer.
  auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<ContendedSliceMetaEngine>>();
  auto *meta_probe = meta.get();
  auto data = std::make_unique<swordfs::test::chunk_test::NullDataEngine>();
  auto *data_probe = data.get();
  SwordFsVolume config;
  config.chunk_size = kTestChunkSize;
  config.chunk_type = swordfs::metadata::ChunkType::kChunkSlice;
  const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
  ASSERT_TRUE(status.ok()) << status.message();

  RunInTestFiber([&] {
    constexpr size_t kChunks = 20;
    const std::string payload(kChunks * kTestChunkSize, 'S');
    FileReadWriter writer(42, kChunks);
    ASSERT_TRUE(writer.Write(Buf(payload), 0).ok());

    // Suspend the first physical PUT. Other Chunks must still upload before
    // that PUT completes: the file-wide publication lock must not cover S3.
    folly::fibers::Baton put_started;
    folly::fibers::Baton release_put;
    folly::fibers::Baton flush_done;
    data_probe->BlockNextPut(&put_started, &release_put);
    Status flush_status;
    auto &fm = folly::fibers::FiberManager::getFiberManager();
    fm.addTask([&] {
      flush_status = writer.Flush();
      flush_done.post();
    });
    put_started.wait();
    for (int i = 0; i < 8 && data_probe->put_calls < 2; ++i) {
      folly::fibers::yield();
    }
    const int uploads_while_first_put_blocked = data_probe->put_calls;
    release_put.post();
    flush_done.wait();
    ASSERT_TRUE(flush_status.ok()) << flush_status.message();
    EXPECT_GT(uploads_while_first_put_blocked, 1);
    // An independent owner must serialize file-wide metadata finalization;
    // concurrent uploads are allowed, but metadata transactions must not
    // invalidate one another's EOF preconditions within the same inode.
    EXPECT_EQ(meta_probe->peak_finalizations_, 1U);

    // Rewrite already-attached indices: FinalizeAttachedWrite, not the first
    // AttachPrepared, now owns every conditional inode metadata transaction.
    std::vector<swordfs::metadata::ChunkID> attached_ids;
    attached_ids.reserve(kChunks);
    for (size_t index = 0; index < kChunks; ++index) {
      swordfs::metadata::FileChunkSnapshot snapshot;
      ASSERT_TRUE(meta_probe->ReadFileChunkSnapshot(42, index, &snapshot).ok());
      ASSERT_TRUE(snapshot.chunk_id.has_value());
      attached_ids.push_back(*snapshot.chunk_id);
    }

    const std::string rewrite(kChunks * kTestChunkSize, 'R');
    ASSERT_TRUE(writer.Write(Buf(rewrite), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    EXPECT_EQ(meta_probe->peak_attached_finalizations_, 1U);
    swordfs::metadata::ChunkMetadataPtr chunk_metadata;
    ASSERT_TRUE(meta_probe->OpenChunkMetadata(swordfs::metadata::ChunkType::kChunkSlice, &chunk_metadata).ok());
    auto slice_metadata = std::dynamic_pointer_cast<swordfs::metadata::slice::SliceChunkMetadata>(chunk_metadata);
    ASSERT_NE(slice_metadata, nullptr);
    for (size_t index = 0; index < kChunks; ++index) {
      swordfs::metadata::FileChunkSnapshot snapshot;
      ASSERT_TRUE(meta_probe->ReadFileChunkSnapshot(42, index, &snapshot).ok());
      ASSERT_EQ(snapshot.chunk_id, attached_ids[index]);
      std::vector<swordfs::chunk::slice::SliceRecord> records;
      ASSERT_TRUE(slice_metadata->ReadRecords(attached_ids[index], &records).ok());
      EXPECT_EQ(records.size(), 2U) << "retry must not append duplicate Slice records";
    }

    FileReadWriter cold_reader(42, kChunks);
    auto out = folly::IOBuf::create(rewrite.size());
    ASSERT_TRUE(cold_reader.Read(rewrite.size(), 0, out.get()).ok());
    EXPECT_EQ(out->length(), rewrite.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), rewrite);
  });
}

// Retain the cross-chunk FileReadWriter contract under the supported Slice
// mechanism, independent of any retired private-index fixtures.
class SliceFileReadWriterMetaEngine : public swordfs::test::chunk_test::MissingMetaEngine {
 public:
  Status OpenChunkMetadata(swordfs::metadata::ChunkType type, swordfs::metadata::ChunkMetadataPtr *out) override {
    if (out == nullptr || type != swordfs::metadata::ChunkType::kChunkSlice) {
      return Status::InvalidArgument("expected Slice metadata");
    }
    *out = slice_metadata_;
    return Status::OK();
  }

  Status GetInode(InodeID ino, SwordFsInode *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("inode output is null");
    }
    swordfs::metadata::FileChunkSnapshot snapshot;
    auto status = ReadFileChunkSnapshot(ino, 0, &snapshot);
    if (status.ok()) {
      *out = snapshot.inode;
    }
    return status;
  }

 private:
  swordfs::metadata::ChunkMetadataPtr slice_metadata_ =
      std::make_shared<swordfs::test::chunk_test::TestSliceChunkMetadata>();
};

class FileReadWriterSliceContractTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RunInTestFiber([&] { swordfs::vfs::InodeHandleManager::Instance().Initialize(); });
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<SliceFileReadWriterMetaEngine>>();
    auto data = std::make_unique<swordfs::test::chunk_test::NullDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    SwordFsVolume config;
    config.chunk_type = swordfs::metadata::ChunkType::kChunkSlice;
    config.chunk_size = kTestChunkSize;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  }

  void TearDown() override {
    RunInTestFiber([&] { swordfs::vfs::InodeHandleManager::Instance().Initialize(); });
    swordfs::volume::VolumeImpl::Initialize();
  }

  static constexpr InodeID kIno = 42;
  SliceFileReadWriterMetaEngine *meta_ = nullptr;
  swordfs::test::chunk_test::NullDataEngine *data_ = nullptr;
};

TEST_F(FileReadWriterSliceContractTest, DirtyCrossChunkReadsRespectOffsetAndLogicalEof) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    const auto payload = Repeat('a', kTestChunkSize) + Repeat('b', 200);
    ASSERT_TRUE(writer.Write(Buf(payload), 0).ok());
    auto out = folly::IOBuf::create(400);
    ASSERT_TRUE(writer.Read(400, kTestChunkSize - 100, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()),
              Repeat('a', 100) + Repeat('b', 200));
    auto past_eof = folly::IOBuf::create(1);
    ASSERT_TRUE(writer.Read(1, payload.size(), past_eof.get()).ok());
    EXPECT_EQ(past_eof->length(), 0U);
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(FileReadWriterSliceContractTest, ExactChunkBoundaryReadEndsWithoutFetchingNextChunk) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf(Repeat('A', kTestChunkSize) + "B"), 0).ok());
    auto first = folly::IOBuf::create(kTestChunkSize);
    ASSERT_TRUE(writer.Read(kTestChunkSize, 0, first.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first->data()), first->length()),
              Repeat('A', kTestChunkSize));
    auto second = folly::IOBuf::create(1);
    ASSERT_TRUE(writer.Read(1, kTestChunkSize, second.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second->data()), second->length()), "B");
    EXPECT_EQ(data_->get_calls, 0);
  });
}

TEST_F(FileReadWriterSliceContractTest, SparseWriteAndColdReadPreserveZerosAcrossChunks) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("AB"), kTestChunkSize + 5).ok());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(kTestChunkSize + 7);
    ASSERT_TRUE(cold.Read(kTestChunkSize + 7, 0, out.get()).ok());
    EXPECT_EQ(out->length(), kTestChunkSize + 7);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()),
              std::string(kTestChunkSize + 5, '\0') + "AB");
  });
}

TEST_F(FileReadWriterSliceContractTest, FlushedRewriteKeepsUntouchedBytesForColdReader) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("abcdefgh"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_TRUE(writer.Write(Buf("XY"), 3).ok());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(cold.Read(8, 0, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "abcXYfgh");
  });
}

TEST_F(FileReadWriterSliceContractTest, ReopenedWriterAppendsWithinPreviouslyFlushedChunk) {
  RunInTestFiber([&] {
    FileReadWriter first(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(first.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(first.Flush().ok());

    FileReadWriter reopened_writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(reopened_writer.Write(Buf(" world"), 5).ok());
    ASSERT_TRUE(reopened_writer.Flush().ok());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "hello world");
    swordfs::metadata::FileChunkSnapshot snapshot;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(kIno, 0, &snapshot).ok());
    EXPECT_EQ(snapshot.inode.attr.size, 11U);
  });
}

TEST_F(FileReadWriterSliceContractTest, InvalidOffsetsAndEmptyReadsCannotPublishData) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(4);
    EXPECT_EQ(writer.Read(0, -1, out.get()).ToErrno(), EINVAL);
    EXPECT_EQ(writer.Read(1, -1, out.get()).ToErrno(), EINVAL);
    EXPECT_EQ(writer.Write(Buf("x"), -1).ToErrno(), EINVAL);
    EXPECT_TRUE(writer.Read(0, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 0U);
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(FileReadWriterSliceContractTest, FailedFlushRetainsDirtyBytesAndCanRetry) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("persistent"), 0).ok());
    data_->next_put_status = Status::IOError("object upload unavailable");
    EXPECT_EQ(writer.Flush().ToErrno(), EIO);
    SwordFsInode live;
    ASSERT_TRUE(writer.GetAttr(&live).ok());
    EXPECT_EQ(live.attr.size, 10U);
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(10);
    ASSERT_TRUE(cold.Read(10, 0, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "persistent");
  });
}

TEST_F(FileReadWriterSliceContractTest, MetadataLookupFailurePropagatesWithoutInventingSparseData) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("saved"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    meta_->file_snapshot_status = Status::IOError("metadata is unavailable");
    auto out = folly::IOBuf::create(5);
    EXPECT_EQ(cold.Read(5, 0, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0U);
    meta_->file_snapshot_status = Status::OK();
    ASSERT_TRUE(cold.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "saved");
  });
}

TEST_F(FileReadWriterSliceContractTest, FailedMetadataLookupPreventsWriteAcceptance) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    meta_->file_snapshot_status = Status::IOError("metadata lookup unavailable");
    EXPECT_EQ(writer.Write(Buf("data"), 0).ToErrno(), EIO);
    EXPECT_EQ(data_->put_calls, 0);

    meta_->file_snapshot_status = Status::OK();
    ASSERT_TRUE(writer.Write(Buf("data"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(4);
    ASSERT_TRUE(reader.Read(4, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "data");
  });
}

TEST_F(FileReadWriterSliceContractTest, RepeatedCleanFlushDoesNotRepublishImmutableObjects) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("unchanged"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    const int uploaded = data_->put_calls;
    ASSERT_GT(uploaded, 0);
    ASSERT_TRUE(writer.Flush().ok());
    EXPECT_EQ(data_->put_calls, uploaded);

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(9);
    ASSERT_TRUE(reader.Read(9, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "unchanged");
  });
}

TEST_F(FileReadWriterSliceContractTest, FailedChunkUploadDoesNotPreventIndependentChunkPublication) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    const std::string content = Repeat('A', kTestChunkSize) + Repeat('B', kTestChunkSize);
    ASSERT_TRUE(writer.Write(Buf(content), 0).ok());

    // Fail exactly one immutable upload. The other chunk must still be
    // published, and the next Flush must recover the full file.
    data_->next_put_status = Status::IOError("first upload failed");
    EXPECT_EQ(writer.Flush().ToErrno(), EIO);
    EXPECT_EQ(data_->put_calls, 2);
    // Two PUT attempts alone cannot prove publication. Before retrying,
    // exactly the successful chunk must already have a durable attachment.
    std::optional<ChunkIndex> published_index;
    for (ChunkIndex index = 0; index < 2; ++index) {
      swordfs::metadata::FileChunkSnapshot snapshot;
      ASSERT_TRUE(meta_->ReadFileChunkSnapshot(kIno, index, &snapshot).ok());
      if (snapshot.chunk_id.has_value()) {
        EXPECT_FALSE(published_index.has_value()) << "both chunks published despite one failed PUT";
        published_index = index;
      }
    }
    ASSERT_TRUE(published_index.has_value()) << "other chunk must publish despite independent upload failure";
    FileReadWriter partially_published_reader(kIno, kMaxParallelFlushes);
    auto published_bytes = folly::IOBuf::create(kTestChunkSize);
    ASSERT_TRUE(
        partially_published_reader.Read(kTestChunkSize, *published_index * kTestChunkSize, published_bytes.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(published_bytes->data()), published_bytes->length()),
              Repeat(*published_index == 0 ? 'A' : 'B', kTestChunkSize));

    ASSERT_TRUE(writer.Flush().ok());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(content.size());
    ASSERT_TRUE(reader.Read(content.size(), 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), content);
  });
}

TEST_F(FileReadWriterSliceContractTest, WritesAreVisibleAcrossOpenHandlesBeforeAndAfterFlush) {
  RunInTestFiber([&] {
    std::shared_ptr<swordfs::vfs::FileHandle> first;
    std::shared_ptr<swordfs::vfs::FileHandle> second;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, O_RDWR, &first).ok());
    auto close_first = folly::makeGuard([&] { (void)first->Release(); });
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, O_RDWR, &second).ok());
    auto close_second = folly::makeGuard([&] { (void)second->Release(); });

    ASSERT_TRUE(first->Write(Buf("shared"), 3).ok());
    auto dirty = folly::IOBuf::create(6);
    ASSERT_TRUE(second->Read(6, 3, dirty.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(dirty->data()), dirty->length()), "shared");
    ASSERT_EQ(data_->put_calls, 0);

    ASSERT_TRUE(first->Flush().ok());
    auto persisted = folly::IOBuf::create(9);
    ASSERT_TRUE(second->Read(9, 0, persisted.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(persisted->data()), persisted->length()),
              std::string(3, '\0') + "shared");
  });
}

// Mechanism-neutral data and FileMetadata doubles for the Slice regressions.
class MockDataEngine : public IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    ++put_calls;
    if (put_started_ != nullptr) {
      auto *started = put_started_;
      auto *release = put_release_;
      put_started_ = nullptr;
      put_release_ = nullptr;
      started->post();
      release->wait();
    } else if (concurrent_put_started_ != nullptr) {
      auto *started = concurrent_put_started_;
      concurrent_put_started_ = nullptr;
      started->post();
    }
    const bool matches_key = fail_put_key.empty() || key == fail_put_key;
    const bool matches_prefix = fail_put_prefix.empty() || key.starts_with(fail_put_prefix);
    if (remaining_forced_put_failures > 0) {
      --remaining_forced_put_failures;
      return put_status;
    }
    if (!put_status.ok() && matches_key && matches_prefix) {
      return put_status;
    }
    store_[std::string(key)] = std::string(reinterpret_cast<const char *>(data->data()), data->length());
    return Status::OK();
  }

  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) override {
    if (get_started_ != nullptr) {
      auto *started = get_started_;
      auto *release = get_release_;
      get_started_ = nullptr;
      get_release_ = nullptr;
      started->post();
      release->wait();
    } else if (concurrent_get_started_ != nullptr) {
      auto *started = concurrent_get_started_;
      concurrent_get_started_ = nullptr;
      started->post();
    }

    if (!get_status.ok()) {
      if (!get_error_payload.empty()) {
        if (out->tailroom() < get_error_payload.size()) {
          return Status::InvalidArgument("injected error payload exceeds output buffer");
        }
        std::memcpy(out->writableTail(), get_error_payload.data(), get_error_payload.size());
        out->append(get_error_payload.size());
      }
      return get_status;
    }

    auto it = store_.find(std::string(key));
    if (it == store_.end()) {
      return Status::NotFound("chunk not found");
    }
    const std::string &chunk = it->second;
    if (offset >= chunk.size()) {
      return Status::OK();
    }
    size_t len = (size == 0) ? chunk.size() - offset : std::min(size, chunk.size() - offset);
    std::memcpy(out->writableTail(), chunk.data() + offset, len);
    out->append(len);
    return Status::OK();
  }

  Status Delete(std::string_view key) override {
    delete_calls.push_back(std::string(key));
    store_.erase(std::string(key));
    return delete_status;
  }

  // Public for tests: the chunks the data engine knows about. Useful
  // for verifying that Truncate's data-engine Deletes match the chunks
  // the metadata engine removed from its chunk map.
  std::vector<std::string> StoredKeys() const {
    std::vector<std::string> out;
    out.reserve(store_.size());
    for (const auto &[k, _] : store_) {
      out.push_back(k);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  void BlockNextGet(folly::fibers::Baton *started, folly::fibers::Baton *release,
                    folly::fibers::Baton *concurrent_get_started = nullptr) {
    get_started_ = started;
    get_release_ = release;
    concurrent_get_started_ = concurrent_get_started;
  }

  void BlockNextPut(folly::fibers::Baton *started, folly::fibers::Baton *release,
                    folly::fibers::Baton *concurrent_put_started = nullptr) {
    put_started_ = started;
    put_release_ = release;
    concurrent_put_started_ = concurrent_put_started;
  }

  std::vector<std::string> delete_calls;
  Status delete_status = Status::OK();
  Status get_status = Status::OK();
  std::string get_error_payload;
  Status put_status = Status::OK();
  std::string fail_put_key;
  std::string fail_put_prefix;
  int put_calls = 0;
  int remaining_forced_put_failures = 0;

 private:
  std::unordered_map<std::string, std::string> store_;
  folly::fibers::Baton *get_started_{nullptr};
  folly::fibers::Baton *get_release_{nullptr};
  folly::fibers::Baton *concurrent_get_started_{nullptr};
  folly::fibers::Baton *put_started_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
  folly::fibers::Baton *concurrent_put_started_{nullptr};
};

// ────────────────────────────────────────────────────────────────
// MockMetaEngine — minimal IMetaEngine with chunk support
// ────────────────────────────────────────────────────────────────

// Common FileMetadata transaction and concurrency fake for Slice tests.
class MockFileMetadataEngine : public IMetaEngine {
 public:
  Status OpenChunkMetadata(swordfs::metadata::ChunkType type, swordfs::metadata::ChunkMetadataPtr *out) override {
    if (out == nullptr || type != swordfs::metadata::ChunkType::kChunkSlice) {
      return Status::InvalidArgument("unexpected test mechanism");
    }
    *out = slice_metadata_;
    return Status::OK();
  }

  Status ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, swordfs::metadata::FileChunkSnapshot *out) override {
    ++find_chunk_calls_;
    if (out == nullptr) {
      return Status::InvalidArgument("missing file chunk snapshot output");
    }
    if (!find_chunk_status_.ok() && (!find_chunk_error_idx_.has_value() || *find_chunk_error_idx_ == index)) {
      return find_chunk_status_;
    }
    swordfs::metadata::FileChunkSnapshot snapshot;
    auto status = GetInode(ino, &snapshot.inode);
    if (!status.ok()) {
      return status;
    }
    const auto &refs = typed_chunks_[ino];
    if (auto it = refs.find(index); it != refs.end()) {
      snapshot.chunk_id = it->second;
    }
    swordfs::metadata::ChunkSizeLayout layout;
    status = swordfs::metadata::PlanChunkSizeLayout(snapshot.inode.attr.size, kTestChunkSize, &layout);
    if (!status.ok()) {
      return status;
    }
    if (layout.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> boundary_id;
      if (auto it = refs.find(layout.boundary->index); it != refs.end()) {
        boundary_id = it->second;
      }
      snapshot.eof_boundary = swordfs::metadata::ChunkBoundarySnapshot{
          .index = layout.boundary->index, .chunk_id = boundary_id, .visible_prefix = layout.boundary->visible_prefix};
    }
    *out = std::move(snapshot);
    return Status::OK();
  }

  Status ReadFileMappingSnapshot(InodeID ino, swordfs::metadata::FileMappingSnapshot *out) override {
    if (std::exchange(fail_next_mapping_snapshot_, false)) {
      return Status::IOError("authoritative size readback failed");
    }
    if (out == nullptr) {
      return Status::InvalidArgument("missing file mapping snapshot output");
    }
    swordfs::metadata::FileMappingSnapshot snapshot;
    auto status = GetInode(ino, &snapshot.inode);
    if (!status.ok()) {
      return status;
    }
    for (const auto &[index, id] : typed_chunks_[ino]) {
      snapshot.mappings.push_back({.index = index, .chunk_id = id});
    }
    *out = std::move(snapshot);
    return Status::OK();
  }

  Status ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<swordfs::metadata::ChunkID> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("missing attachment output");
    }
    out->reset();
    const auto &refs = typed_chunks_[ino];
    if (auto it = refs.find(index); it != refs.end()) {
      *out = it->second;
    }
    return Status::OK();
  }

  Status CheckFileState(InodeID ino, const swordfs::metadata::FileSizePrecondition &expected) {
    auto status = swordfs::metadata::ValidateFileSizePrecondition(expected, kTestChunkSize);
    if (!status.ok()) {
      return status;
    }
    if (expected.eof != static_cast<uint64_t>(file_size_)) {
      return Status::AlreadyExists("observed EOF changed");
    }
    if (expected.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> actual;
      status = ProbeAttachment(ino, expected.boundary->index, &actual);
      if (!status.ok()) {
        return status;
      }
      if (actual != expected.boundary->chunk_id) {
        return Status::AlreadyExists("observed EOF boundary changed");
      }
    }
    return Status::OK();
  }

  Status AttachPrepared(InodeID ino, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                        const swordfs::metadata::FileSizePrecondition &expected) override {
    PauseNextPublicationIfRequested();
    if (!attach_prepared_status_.ok() && !attach_prepared_commit_on_error_ && !attach_prepared_size_only_on_error_ &&
        !attach_prepared_mapping_only_on_error_) {
      return attach_prepared_status_;
    }
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, kTestChunkSize);
    if (!status.ok()) {
      return status;
    }
    status = CheckFileState(ino, expected);
    if (!status.ok()) {
      return status;
    }
    auto &refs = typed_chunks_[ino];
    if (attach_prepared_size_only_on_error_) {
      attach_prepared_size_only_on_error_ = false;
      const auto failure = std::exchange(attach_prepared_status_, Status::OK());
      file_size_ = std::max<uint64_t>(file_size_, end);
      return failure;
    }
    if (!refs.emplace(index, id).second) {
      return Status::AlreadyExists("first attachment already exists");
    }
    if (attach_prepared_mapping_only_on_error_) {
      attach_prepared_mapping_only_on_error_ = false;
      return std::exchange(attach_prepared_status_, Status::OK());
    }
    file_size_ = std::max<uint64_t>(file_size_, end);
    return attach_prepared_status_;
  }

  Status FinalizeAttachedWrite(InodeID ino, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                               const swordfs::metadata::FileSizePrecondition &expected) override {
    PauseNextPublicationIfRequested();
    if (!finalize_attached_status_.ok() && !finalize_attached_commit_on_error_) {
      return finalize_attached_status_;
    }
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, kTestChunkSize);
    if (!status.ok()) {
      return status;
    }
    status = CheckFileState(ino, expected);
    if (!status.ok()) {
      return status;
    }
    auto &refs = typed_chunks_[ino];
    const auto it = refs.find(index);
    if (it == refs.end() || it->second != id) {
      return Status::AlreadyExists("attached ChunkID changed");
    }
    file_size_ = std::max<uint64_t>(file_size_, end);
    return finalize_attached_status_;
  }

  Status CommitShrink(InodeID ino, const swordfs::metadata::ChunkSizePlan &plan, const SwordFsAttr &attr,
                      SetAttrField fields, swordfs::metadata::ChunkSizeCommitResult *out) override {
    return CommitSize(ino, plan, attr, fields, /*shrink=*/true, out);
  }
  Status CommitGrow(InodeID ino, const swordfs::metadata::ChunkSizePlan &plan, const SwordFsAttr &attr,
                    SetAttrField fields, swordfs::metadata::ChunkSizeCommitResult *out) override {
    return CommitSize(ino, plan, attr, fields, /*shrink=*/false, out);
  }

  Status CommitSize(InodeID ino, const swordfs::metadata::ChunkSizePlan &plan, const SwordFsAttr &attr,
                    SetAttrField fields, bool shrink, swordfs::metadata::ChunkSizeCommitResult *out) {
    ++size_commit_calls_;
    if (!out || !HasSetAttrField(fields, SetAttrField::kSize) || plan.target_eof != attr.size) {
      return Status::InvalidArgument("invalid typed file size commit request");
    }
    swordfs::metadata::FileMappingSnapshot snapshot;
    auto status = ReadFileMappingSnapshot(ino, &snapshot);
    if (!status.ok()) {
      return status;
    }
    swordfs::metadata::ChunkSizePlan current;
    status = swordfs::metadata::ValidateSizeCommitPlan(plan, snapshot.inode.attr.size, kTestChunkSize,
                                                       snapshot.mappings, shrink, &current);
    if (!status.ok()) {
      return status;
    }
    if (!truncate_status_.ok() && !truncate_commit_on_error_) {
      return truncate_status_;
    }
    if (!set_attr_status_.ok() && !set_attr_commit_on_error_) {
      return set_attr_status_;
    }
    auto &refs = typed_chunks_[ino];
    out->detached.clear();
    swordfs::metadata::ChunkSizeLayout layout;
    status = swordfs::metadata::PlanChunkSizeLayout(attr.size, kTestChunkSize, &layout);
    if (!status.ok()) {
      return status;
    }
    if (shrink) {
      for (auto it = refs.begin(); it != refs.end();) {
        if (layout.ShouldDetach(it->first)) {
          out->detached.push_back({.index = it->first, .chunk_id = it->second});
          if (preserve_detach_on_error_ && truncate_commit_on_error_ && !truncate_status_.ok()) {
            ++it;
            continue;
          }
          it = refs.erase(it);
        } else {
          ++it;
        }
      }
    }
    file_size_ = attr.size;
    out->inode = snapshot.inode;
    out->inode.attr.size = attr.size;
    out->boundary = current.boundary;
    if (!truncate_status_.ok() && truncate_commit_on_error_) {
      fail_next_mapping_snapshot_ = fail_snapshot_after_commit_;
      return truncate_status_;
    }
    if (!set_attr_status_.ok() && set_attr_commit_on_error_) {
      fail_next_mapping_snapshot_ = fail_snapshot_after_commit_;
      return set_attr_status_;
    }
    return Status::OK();
  }
  Status Initialize() override {
    return Status::OK();
  }
  Status FormatVolume(const SwordFsVolume &) override {
    return Status::OK();
  }
  Status LoadVolume(SwordFsVolume *) override {
    return Status::OK();
  }
  Limits GetLimits() const override {
    return {};
  }
  Status Lookup(InodeID, std::string_view, SwordFsInode *out) override {
    if (out) {
      *out = {};
    }
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    if (!get_inode_status_.ok()) {
      return get_inode_status_;
    }
    const uint64_t snapshot_size = reported_file_size_.value_or(static_cast<uint64_t>(file_size_));
    if (get_inode_started_ != nullptr) {
      auto *started = get_inode_started_;
      auto *release = get_inode_release_;
      get_inode_started_ = nullptr;
      get_inode_release_ = nullptr;
      started->post();
      release->wait();
    }
    if (out) {
      *out = SwordFsInode(ino, SwordFsAttr(ino, S_IFREG | 0644), /*parent_ino=*/1);
      out->attr.size = snapshot_size;
      out->attr.inode_flags = test_inode_flags_;
    }
    return Status::OK();
  }
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("inode batch output is null");
    }
    out->clear();
    for (const InodeID requested_ino : inode_ids) {
      SwordFsInode inode;
      auto status = GetInode(requested_ino, &inode);
      if (!status.ok()) {
        return status;
      }
      out->emplace_back(std::move(inode));
    }
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkNod(InodeID, std::string_view, uint32_t, uint64_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view, std::optional<InodeID> = std::nullopt) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag) override {
    return Status::OK();
  }
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override {
    ++set_attr_calls_;
    last_set_attr_fields_ = fields;
    if (reject_legacy_size_setattr_ && HasSetAttrField(fields, SetAttrField::kSize)) {
      return Status::InvalidArgument("direct metadata size setattr is forbidden");
    }
    if (!set_attr_status_.ok() && !set_attr_commit_on_error_) {
      return set_attr_status_;
    }
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      file_size_ = static_cast<off_t>(attr.size);
    }
    if (out) {
      *out = {};
      out->attr.size = file_size_;
    }
    return set_attr_status_;
  }
  Status SetInodeFlags(InodeID, swordfs::metadata::InodeFlag, SwordFsInode *) override {
    return Status::NotSupported("inode flags");
  }
  Status SetXAttr(InodeID, std::string_view, std::string_view, swordfs::metadata::XAttrSetMode) override {
    return Status::NotSupported("xattr");
  }
  Status GetXAttr(InodeID, std::string_view, std::string *) override {
    return Status::NotSupported("xattr");
  }
  Status ListXAttrs(InodeID, std::vector<std::string> *) override {
    return Status::NotSupported("xattr");
  }
  Status RemoveXAttr(InodeID, std::string_view) override {
    return Status::NotSupported("xattr");
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID, uint64_t *size = nullptr, swordfs::metadata::InodeFlag * = nullptr) override {
    if (size != nullptr) {
      *size = static_cast<uint64_t>(file_size_);
    }
    return Status::OK();
  }
  Status PrepareReclaim(InodeID) override {
    return Status::OK();
  }
  Status CompleteReclaim(InodeID) override {
    return Status::OK();
  }
  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingDeletesBatch(size_t max_items, const swordfs::metadata::PendingDeleteVisitorFn &visitor,
                                  bool *has_more) override {
    std::vector<swordfs::metadata::PendingDelete> pending;
    pending.reserve(pending_deletes_.size());
    for (const auto &[key, work] : pending_deletes_) {
      (void)key;
      pending.push_back(work);
    }
    *has_more = pending.size() > max_items;
    const size_t count = std::min(max_items, pending.size());
    for (size_t i = 0; i < count; ++i) {
      auto status = visitor(pending[i]);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }
  Status RegisterPendingDelete(const swordfs::metadata::PendingDelete &work) override {
    pending_deletes_.insert_or_assign(work.id, work);
    return Status::OK();
  }
  Status CompletePendingDelete(std::string_view key) override {
    complete_pending_delete_calls_.emplace_back(key);
    if (!complete_pending_delete_status_.ok()) {
      return complete_pending_delete_status_;
    }
    pending_deletes_.erase(std::string(key));
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }

  void DetachTypedChunkForTest(InodeID ino, ChunkIndex index) {
    typed_chunks_[ino].erase(index);
  }

  void set_file_size(off_t size) {
    file_size_ = size;
  }
  void set_reported_file_size(uint64_t size) {
    reported_file_size_ = size;
  }
  void set_truncate_status(Status s) {
    truncate_status_ = s;
  }
  off_t file_size() const {
    return file_size_;
  }

  int MetadataMutationCalls() const {
    return set_attr_calls_ + size_commit_calls_;
  }

  int SizeCommitCalls() const {
    return size_commit_calls_;
  }

  void BlockNextCommit(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    commit_started_ = started;
    commit_release_ = release;
  }

  void PauseNextPublicationIfRequested() {
    if (commit_started_ == nullptr) {
      return;
    }
    auto *started = std::exchange(commit_started_, nullptr);
    auto *release = std::exchange(commit_release_, nullptr);
    started->post();
    release->wait();
  }

  void BlockNextGetInode(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    get_inode_started_ = started;
    get_inode_release_ = release;
  }

  int find_chunk_calls_ = 0;
  std::vector<std::string> complete_pending_delete_calls_;
  Status complete_pending_delete_status_ = Status::OK();
  Status attach_prepared_status_ = Status::OK();
  bool attach_prepared_commit_on_error_ = false;
  bool attach_prepared_size_only_on_error_ = false;
  bool attach_prepared_mapping_only_on_error_ = false;
  Status finalize_attached_status_ = Status::OK();
  bool finalize_attached_commit_on_error_ = false;
  swordfs::metadata::InodeFlag test_inode_flags_ = swordfs::metadata::InodeFlag::kNone;
  Status set_attr_status_ = Status::OK();
  bool reject_legacy_size_setattr_ = false;
  SetAttrField last_set_attr_fields_ = static_cast<SetAttrField>(0);
  bool set_attr_commit_on_error_ = false;
  bool truncate_commit_on_error_ = false;
  bool fail_snapshot_after_commit_ = false;
  bool preserve_detach_on_error_ = false;
  Status find_chunk_status_ = Status::OK();
  Status get_inode_status_ = Status::OK();
  std::optional<ChunkIndex> find_chunk_error_idx_;

 protected:
  std::unordered_map<InodeID, std::map<ChunkIndex, swordfs::metadata::ChunkID>> typed_chunks_;

 private:
  int set_attr_calls_ = 0;
  int size_commit_calls_ = 0;
  off_t file_size_ = 0;
  swordfs::metadata::ChunkMetadataPtr slice_metadata_ =
      std::make_shared<swordfs::test::chunk_test::TestSliceChunkMetadata>();
  std::optional<uint64_t> reported_file_size_;
  Status truncate_status_ = Status::OK();
  bool fail_next_mapping_snapshot_ = false;
  std::unordered_map<std::string, swordfs::metadata::PendingDelete> pending_deletes_;
  folly::fibers::Baton *get_inode_started_{nullptr};
  folly::fibers::Baton *get_inode_release_{nullptr};
  folly::fibers::Baton *commit_started_{nullptr};
  folly::fibers::Baton *commit_release_{nullptr};
};

// Reuse the FileMetadata transaction double for whole-file contracts, which
// the chunk-focused MissingMetaEngine intentionally does not implement.
class FileReadWriterSliceSizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<MockFileMetadataEngine>>();
    auto data = std::make_unique<MockDataEngine>();
    meta_ = meta.get();
    data_ = data.get();
    SwordFsVolume config;
    config.chunk_type = swordfs::metadata::ChunkType::kChunkSlice;
    config.chunk_size = kTestChunkSize;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  }

  void TearDown() override {
    swordfs::volume::VolumeImpl::Initialize();
  }

  static constexpr InodeID kIno = 42;
  MockFileMetadataEngine *meta_ = nullptr;
  MockDataEngine *data_ = nullptr;
};

TEST_F(FileReadWriterSliceSizeTest, CorruptedPersistedEofFailsClosedBeforeChunkRead) {
  RunInTestFiber([&] {
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    meta_->set_reported_file_size(swordfs::metadata::kMaxSupportedFileSize + 1);
    auto out = folly::IOBuf::create(1);
    EXPECT_EQ(reader.Read(1, 0, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0U);
  });
}

TEST_F(FileReadWriterSliceSizeTest, CorruptedPriorEofCannotEnterTruncatePlanningOrCommit) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    const uint64_t corrupt_eof = swordfs::metadata::kMaxSupportedFileSize + 1;
    meta_->set_reported_file_size(corrupt_eof);
    const int mutation_calls = meta_->MetadataMutationCalls();
    EXPECT_EQ(writer.Truncate(32).ToErrno(), EINVAL);
    EXPECT_EQ(meta_->MetadataMutationCalls(), mutation_calls);
    swordfs::metadata::FileMappingSnapshot snapshot;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &snapshot).ok());
    EXPECT_EQ(snapshot.inode.attr.size, corrupt_eof);
  });
}

TEST_F(FileReadWriterSliceSizeTest, FailedMappingSnapshotStopsTruncateBeforeMutation) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    meta_->set_file_size(64);
    const int mutation_calls = meta_->MetadataMutationCalls();
    meta_->get_inode_status_ = Status::IOError("mapping snapshot unavailable");
    EXPECT_EQ(writer.Truncate(32).ToErrno(), EIO);
    EXPECT_EQ(meta_->file_size(), 64);
    EXPECT_EQ(meta_->MetadataMutationCalls(), mutation_calls);
  });
}

TEST_F(FileReadWriterSliceSizeTest, OverflowingWriteDoesNotMutateMetadataOrUploadData) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    const auto offset = std::numeric_limits<off_t>::max() - 1;
    EXPECT_EQ(writer.Write(Buf("xx"), offset).ToErrno(), EINVAL);
    EXPECT_EQ(meta_->file_size(), 0);
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(FileReadWriterSliceSizeTest, InodeMetadataReadFailurePropagatesWithoutReturningSparseBytes) {
  RunInTestFiber([&] {
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    meta_->get_inode_status_ = Status::IOError("inode lookup unavailable");
    auto out = folly::IOBuf::create(8);
    EXPECT_EQ(reader.Read(8, 0, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0U);
  });
}

TEST_F(FileReadWriterSliceSizeTest, InvalidSizeMutationsNeverEnterMetadataCommit) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    const int mutations = meta_->MetadataMutationCalls();
    const uint64_t invalid = swordfs::metadata::kMaxSupportedFileSize + 1;
    EXPECT_EQ(writer.Truncate(invalid).ToErrno(), EINVAL);
    SwordFsAttr requested;
    requested.size = invalid;
    EXPECT_EQ(writer.SetAttr(requested, SetAttrField::kSize, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(meta_->MetadataMutationCalls(), mutations);
    EXPECT_EQ(meta_->file_size(), 0);
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(FileReadWriterSliceSizeTest, NoopSizeMutationStillUpdatesMtimeWithoutLegacySizeSetAttr) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("uncommitted"), 0).ok());
    ASSERT_TRUE(writer.Truncate(0).ok());
    EXPECT_TRUE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kMtimeNow));
    EXPECT_FALSE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kSize));
    EXPECT_EQ(meta_->SizeCommitCalls(), 0);
    EXPECT_EQ(data_->put_calls, 0);

    meta_->set_attr_status_ = Status::IOError("mtime mutation failed");
    EXPECT_EQ(writer.Truncate(0).ToErrno(), EIO);
  });
}

TEST_F(FileReadWriterSliceSizeTest, NoopSizeMutationPreservesExplicitMtimeSelection) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    SwordFsAttr requested;
    requested.size = 0;
    ASSERT_TRUE(writer.SetAttr(requested, SetAttrField::kSize | SetAttrField::kMtime, nullptr).ok());
    EXPECT_TRUE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kMtime));
    EXPECT_FALSE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kMtimeNow));
    EXPECT_FALSE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kSize));
    ASSERT_TRUE(writer.SetAttr(requested, SetAttrField::kSize | SetAttrField::kMtimeNow, nullptr).ok());
    EXPECT_TRUE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kMtimeNow));
    EXPECT_FALSE(swordfs::metadata::HasSetAttrField(meta_->last_set_attr_fields_, SetAttrField::kMtime));
  });
}

TEST_F(FileReadWriterSliceSizeTest, GrowingEmptyFileDoesNotInventChunkMappings) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Truncate(2 * kTestChunkSize).ok());
    swordfs::metadata::FileMappingSnapshot snapshot;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &snapshot).ok());
    EXPECT_TRUE(snapshot.mappings.empty());
    EXPECT_EQ(snapshot.inode.attr.size, 2 * kTestChunkSize);
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(FileReadWriterSliceSizeTest, SparseCachedWriteReportsBlocksWithoutPublishing) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    const off_t offset = 1024 * kTestChunkSize;
    ASSERT_TRUE(writer.Write(Buf("sparse"), offset).ok());
    SwordFsInode inode;
    ASSERT_TRUE(writer.GetAttr(&inode).ok());
    struct stat attr{};
    inode.attr.ToPosixStat(&attr);
    EXPECT_EQ(attr.st_size, offset + 6);
    EXPECT_GT(attr.st_blocks, 0);
    EXPECT_LT(static_cast<uint64_t>(attr.st_blocks) * 512, static_cast<uint64_t>(attr.st_size));
    EXPECT_EQ(data_->put_calls, 0);
    EXPECT_EQ(meta_->file_size(), 0);
  });
}

TEST_F(FileReadWriterSliceSizeTest, LowerOffsetRewriteFlushNeverShrinksPreviouslyCommittedEof) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("higher"), kTestChunkSize + 9).ok());
    ASSERT_TRUE(writer.Flush().ok());
    const off_t committed = meta_->file_size();
    ASSERT_GT(committed, static_cast<off_t>(kTestChunkSize));
    ASSERT_TRUE(writer.Write(Buf("lower"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    EXPECT_EQ(meta_->file_size(), committed);
    SwordFsInode inode;
    ASSERT_TRUE(writer.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, static_cast<uint64_t>(committed));
  });
}

TEST_F(FileReadWriterSliceSizeTest, HighIndexCachedTruncateNeverAliasesLowIndex) {
  RunInTestFiber([&] {
    constexpr uint64_t index = uint64_t{1} << 32;
    constexpr off_t high_offset = static_cast<off_t>(index * kTestChunkSize);
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("L"), 0).ok());
    ASSERT_TRUE(writer.Write(Buf("H"), high_offset).ok());
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_TRUE(writer.Truncate(1).ok());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto low = folly::IOBuf::create(1);
    ASSERT_TRUE(reader.Read(1, 0, low.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(low->data()), low->length()), "L");
    EXPECT_EQ(meta_->file_size(), 1);
    auto high = folly::IOBuf::create(1);
    ASSERT_TRUE(reader.Read(1, high_offset, high.get()).ok());
    EXPECT_EQ(high->length(), 0U);
  });
}

TEST_F(FileReadWriterSliceSizeTest, TruncateAndGrowNeverReviveDiscardedSliceSuffix) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("abcdefghijkl"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_TRUE(writer.Truncate(4).ok());
    ASSERT_TRUE(writer.Truncate(12).ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(cold.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()),
              std::string("abcd\0\0\0\0\0\0\0\0", 12));
  });
}

TEST_F(FileReadWriterSliceSizeTest, FailedTruncateDoesNotCommitNewEof) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("abcdefgh"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    meta_->set_truncate_status(Status::IOError("size commit unavailable"));
    EXPECT_EQ(writer.Truncate(3).ToErrno(), EIO);
    EXPECT_EQ(meta_->file_size(), 8);
    meta_->set_truncate_status(Status::OK());
    ASSERT_TRUE(writer.Truncate(3).ok());
    EXPECT_EQ(meta_->file_size(), 3);
  });
}

TEST_F(FileReadWriterSliceSizeTest, HighOffsetWriteFlushAndColdReadPreserve64BitChunkIndex) {
  RunInTestFiber([&] {
    constexpr off_t kOffset = std::numeric_limits<off_t>::max() - 1;
    constexpr ChunkIndex kIndex = static_cast<uint64_t>(kOffset) / kTestChunkSize;
    static_assert(kIndex > std::numeric_limits<uint32_t>::max());
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("X"), kOffset).ok());
    ASSERT_TRUE(writer.Flush().ok());
    EXPECT_EQ(meta_->file_size(), std::numeric_limits<off_t>::max());
    swordfs::metadata::FileChunkSnapshot snapshot;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(kIno, kIndex, &snapshot).ok());
    ASSERT_TRUE(snapshot.chunk_id.has_value());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(2);
    ASSERT_TRUE(reopened.Read(2, kOffset - 1, out.get()).ok());
    ASSERT_EQ(out->length(), 2U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), std::string("\0X", 2));
  });
}

TEST_F(FileReadWriterSliceSizeTest, AmbiguousCommittedShrinkKeepsDirtyPrefixAndAuthoritativeEof) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_TRUE(writer.Write(Buf("HELLO"), 0).ok());
    meta_->truncate_commit_on_error_ = true;
    meta_->set_truncate_status(Status::OutcomeUnknown("lost committed Slice shrink reply"));
    EXPECT_TRUE(writer.Truncate(5).IsOutcomeUnknown());
    swordfs::metadata::FileMappingSnapshot committed;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &committed).ok());
    EXPECT_EQ(committed.inode.attr.size, 5U);
    ASSERT_EQ(committed.mappings.size(), 1U);
    meta_->truncate_commit_on_error_ = false;
    meta_->set_truncate_status(Status::OK());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(cold.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO");
  });
}

TEST_F(FileReadWriterSliceSizeTest, AmbiguousCommittedSizeSetAttrKeepsDirtyPrefixAndAuthoritativeEof) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_TRUE(writer.Write(Buf("HELLO"), 0).ok());
    SwordFsAttr requested{};
    requested.size = 5;
    meta_->set_attr_commit_on_error_ = true;
    meta_->set_attr_status_ = Status::OutcomeUnknown("lost committed Slice size SetAttr reply");
    EXPECT_TRUE(writer.SetAttr(requested, SetAttrField::kSize, nullptr).IsOutcomeUnknown());
    swordfs::metadata::FileMappingSnapshot committed;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &committed).ok());
    EXPECT_EQ(committed.inode.attr.size, 5U);
    ASSERT_EQ(committed.mappings.size(), 1U);
    meta_->set_attr_commit_on_error_ = false;
    meta_->set_attr_status_ = Status::OK();
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(cold.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO");
  });
}

TEST_F(FileReadWriterSliceSizeTest, KnownPartialExecCommittedShrinkNeverRepublishesDetachedChunk) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf(Repeat('A', kTestChunkSize) + "tail"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    auto cached = folly::IOBuf::create(4);
    ASSERT_TRUE(writer.Read(4, kTestChunkSize, cached.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(cached->data()), cached->length()), "tail");
    meta_->truncate_commit_on_error_ = true;
    meta_->set_truncate_status(Status::IOError("Redis EXEC failed after committed Slice EOF and detach"));
    EXPECT_EQ(writer.Truncate(0).ToErrno(), EIO);
    swordfs::metadata::FileMappingSnapshot committed;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &committed).ok());
    EXPECT_EQ(committed.inode.attr.size, 0U);
    EXPECT_TRUE(committed.mappings.empty());
    auto out = folly::IOBuf::create(4);
    ASSERT_TRUE(writer.Read(4, kTestChunkSize, out.get()).ok());
    EXPECT_EQ(out->length(), 0U);
    SwordFsInode attr;
    ASSERT_TRUE(writer.GetAttr(&attr).ok());
    EXPECT_EQ(attr.attr.size, 0U);
    meta_->truncate_commit_on_error_ = false;
    meta_->set_truncate_status(Status::OK());
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &committed).ok());
    EXPECT_EQ(committed.inode.attr.size, 0U);
    EXPECT_TRUE(committed.mappings.empty());
  });
}

TEST_F(FileReadWriterSliceSizeTest, UnverifiablePartialExecPoisonsAllLiveFileOperations) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("old data"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    meta_->truncate_commit_on_error_ = true;
    meta_->fail_snapshot_after_commit_ = true;
    meta_->set_truncate_status(Status::IOError("Redis EXEC error; authoritative outcome unreadable"));
    EXPECT_EQ(writer.Truncate(0).ToErrno(), EIO);
    EXPECT_EQ(meta_->file_size(), 0);
    auto out = folly::IOBuf::create(8);
    EXPECT_EQ(writer.Read(8, 0, out.get()).ToErrno(), EIO);
    SwordFsInode attr;
    EXPECT_EQ(writer.GetAttr(&attr).ToErrno(), EIO);
    EXPECT_EQ(writer.Write(Buf("new"), 0).ToErrno(), EIO);
    EXPECT_EQ(writer.Flush().ToErrno(), EIO);
    EXPECT_EQ(writer.Truncate(1).ToErrno(), EIO);
    SwordFsAttr requested{};
    requested.size = 1;
    EXPECT_EQ(writer.SetAttr(requested, SetAttrField::kSize, nullptr).ToErrno(), EIO);
    EXPECT_EQ(writer.SetInodeFlags(swordfs::metadata::InodeFlag::kNone, nullptr).ToErrno(), EIO);
  });
}

TEST_F(FileReadWriterSliceSizeTest, PartialExecEofWithUndetachedChunkFailsClosed) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("old data"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    meta_->truncate_commit_on_error_ = true;
    meta_->preserve_detach_on_error_ = true;
    meta_->set_truncate_status(Status::IOError("Redis EXEC error before Slice detach"));
    EXPECT_EQ(writer.Truncate(0).ToErrno(), EIO);
    swordfs::metadata::FileMappingSnapshot partial;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &partial).ok());
    EXPECT_EQ(partial.inode.attr.size, 0U);
    ASSERT_EQ(partial.mappings.size(), 1U);
    auto out = folly::IOBuf::create(8);
    EXPECT_EQ(writer.Read(8, 0, out.get()).ToErrno(), EIO);
    SwordFsInode attr;
    EXPECT_EQ(writer.GetAttr(&attr).ToErrno(), EIO);
    EXPECT_EQ(writer.Flush().ToErrno(), EIO);
  });
}

TEST_F(FileReadWriterSliceSizeTest, FailedFlushKeepsUncommittedLiveSizeVisible) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("unpublished"), 0).ok());
    data_->put_status = Status::IOError("storage unavailable");
    EXPECT_EQ(writer.Flush().ToErrno(), EIO);
    EXPECT_EQ(meta_->file_size(), 0);
    SwordFsInode live;
    ASSERT_TRUE(writer.GetAttr(&live).ok());
    EXPECT_EQ(live.attr.size, 11U);
    data_->put_status = Status::OK();
    ASSERT_TRUE(writer.Flush().ok());
    EXPECT_EQ(meta_->file_size(), 11);
  });
}

TEST_F(FileReadWriterSliceSizeTest, SuccessfulTruncateRetiresTransientSizeOverlay) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("Hello,_World!"), 0).ok());
    ASSERT_TRUE(writer.Truncate(5).ok());
    ASSERT_EQ(meta_->file_size(), 5);

    // Once Truncate commits, an independent authoritative size update must
    // not be masked by a stale local dirty-write EOF upper bound.
    meta_->set_file_size(2);
    SwordFsInode inode;
    ASSERT_TRUE(writer.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, 2U);
  });
}

TEST_F(FileReadWriterSliceSizeTest, SuccessfulFlushRetiresTransientSizeOverlay) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("Hello,_World!"), 0).ok());
    SwordFsInode live;
    ASSERT_TRUE(writer.GetAttr(&live).ok());
    ASSERT_EQ(live.attr.size, 13U);
    ASSERT_TRUE(writer.Flush().ok());
    ASSERT_EQ(meta_->file_size(), 13);
    // A later authoritative metadata change must not be masked by the old
    // dirty-write upper bound once Flush has committed successfully.
    meta_->set_file_size(5);
    ASSERT_TRUE(writer.GetAttr(&live).ok());
    EXPECT_EQ(live.attr.size, 5U);
  });
}

TEST_F(FileReadWriterSliceSizeTest, AmbiguousFirstAttachmentReconcilesBeforeTruncate) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf(Repeat('T', 200)), 0).ok());
    meta_->attach_prepared_status_ = Status::OutcomeUnknown("first attach committed but reply lost");
    meta_->attach_prepared_commit_on_error_ = true;
    ASSERT_TRUE(writer.Flush().ok()) << "committed first attachment must be reconciled";
    swordfs::metadata::FileChunkSnapshot attached;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(kIno, 0, &attached).ok());
    ASSERT_TRUE(attached.chunk_id.has_value());
    meta_->attach_prepared_status_ = Status::OK();
    meta_->attach_prepared_commit_on_error_ = false;

    ASSERT_TRUE(writer.Truncate(64).ok());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(200);
    ASSERT_TRUE(reader.Read(200, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('T', 64));
    EXPECT_EQ(meta_->file_size(), 64);

    // The POSIX truncate/grow zero-fill rule still applies after a lost
    // first-attachment reply. Do not assert COW head/revision internals.
    ASSERT_TRUE(writer.Truncate(200).ok());
    FileReadWriter grown(kIno, kMaxParallelFlushes);
    auto grown_out = folly::IOBuf::create(200);
    ASSERT_TRUE(grown.Read(200, 0, grown_out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(grown_out->data()), grown_out->length()),
              Repeat('T', 64) + std::string(136, '\0'));
  });
}

TEST_F(FileReadWriterSliceSizeTest, CrossChunkMetadataErrorCannotReturnPartialRead) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf(Repeat('a', kTestChunkSize) + "tail"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter cold(kIno, kMaxParallelFlushes);
    meta_->find_chunk_status_ = Status::IOError("second chunk lookup failed");
    meta_->find_chunk_error_idx_ = 1;
    auto out = folly::IOBuf::create(8);
    EXPECT_EQ(cold.Read(8, kTestChunkSize - 4, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0U);
    meta_->find_chunk_status_ = Status::OK();
    ASSERT_TRUE(cold.Read(8, kTestChunkSize - 4, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "aaaatail");
  });
}

// These barriers protect FileReadWriter coordination, not private COW
// revisions. Both workers execute on the same Folly EventBase so a blocked
// storage request cannot hide a thread-level deadlock.
TEST_F(FileReadWriterSliceSizeTest, ForegroundWriteProgressesDuringBlockedFlushUpload) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("AAAA"), 0).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton write_done;
  data_->BlockNextPut(&put_started, &release_put);
  Status flush_status;
  Status write_status;
  fm.addTask([&] {
    flush_status = writer.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    write_status = writer.Write(Buf("BBBB"), 4);
    write_done.post();
  });
  const bool write_progressed = swordfs::test::DriveEventBaseUntil(evb, [&] { return write_done.try_wait(); });
  EXPECT_TRUE(write_progressed) << "foreground writes must not wait for a prior generation's S3 PUT";
  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && write_done.try_wait(); }, "Slice write/flush barrier completion");
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_TRUE(write_status.ok()) << write_status.message();
  RunInTestFiber([&] {
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterSliceSizeTest, ForegroundWriteProgressesDuringBlockedMetadataCommit) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("AAAA"), 0).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton commit_started;
  folly::fibers::Baton release_commit;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton write_done;
  meta_->BlockNextCommit(&commit_started, &release_commit);
  Status flush_status;
  Status write_status;
  fm.addTask([&] {
    flush_status = writer.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    commit_started.wait();
    write_status = writer.Write(Buf("BBBB"), 4);
    write_done.post();
  });
  const bool write_progressed = swordfs::test::DriveEventBaseUntil(evb, [&] { return write_done.try_wait(); });
  EXPECT_TRUE(write_progressed) << "foreground writes cannot wait for a blocked Redis publication";
  release_commit.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && write_done.try_wait(); }, "Slice write/metadata barrier completion");
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_TRUE(write_status.ok()) << write_status.message();
  RunInTestFiber([&] {
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterSliceSizeTest, TruncateCannotOvertakeBlockedFlushPublication) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("AAAA"), 0).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton truncate_started;
  folly::fibers::Baton truncate_probe_done;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton truncate_done;
  data_->BlockNextPut(&put_started, &release_put);
  Status flush_status;
  Status truncate_status;
  fm.addTask([&] {
    flush_status = writer.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    truncate_started.post();
    truncate_status = writer.Truncate(0);
    truncate_done.post();
  });
  fm.addTask([&] {
    truncate_started.wait();
    // The Truncate fiber runs to its next suspension after posting its entry
    // baton. A separately scheduled probe therefore observes an attempted
    // Truncate, not merely an intent to invoke it.
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    truncate_probe_done.post();
  });
  const bool truncate_entered =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return put_started.try_wait() && truncate_probe_done.try_wait(); });
  EXPECT_TRUE(truncate_entered) << "truncate must reach its waiting point while the first PUT is held";
  EXPECT_FALSE(truncate_done.try_wait()) << "truncate cannot commit before blocked upload publication";
  EXPECT_EQ(meta_->MetadataMutationCalls(), 0) << "no size or attribute mutation may overtake the held PUT";
  EXPECT_EQ(meta_->file_size(), 0);
  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && truncate_done.try_wait(); }, "Slice truncate/flush completion");
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_TRUE(truncate_status.ok()) << truncate_status.message();
  EXPECT_EQ(meta_->MetadataMutationCalls(), 1) << "the completed Truncate must reach the real metadata path";
  EXPECT_EQ(meta_->file_size(), 0);
  RunInTestFiber([&] {
    swordfs::metadata::FileMappingSnapshot snapshot;
    ASSERT_TRUE(meta_->ReadFileMappingSnapshot(kIno, &snapshot).ok());
    EXPECT_TRUE(snapshot.mappings.empty());
  });
}

TEST_F(FileReadWriterSliceSizeTest, TruncateWaitsForReadWithoutBlockingEventLoop) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("persisted"), 0).ok());
    ASSERT_TRUE(writer.Flush().ok());
  });
  FileReadWriter reader(kIno, kMaxParallelFlushes);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_started;
  folly::fibers::Baton release_get;
  folly::fibers::Baton truncate_started;
  folly::fibers::Baton truncate_probe_done;
  folly::fibers::Baton read_done;
  folly::fibers::Baton truncate_done;
  data_->BlockNextGet(&get_started, &release_get);
  Status read_status;
  Status truncate_status;
  auto out = folly::IOBuf::create(9);
  fm.addTask([&] {
    read_status = reader.Read(9, 0, out.get());
    read_done.post();
  });
  fm.addTask([&] {
    get_started.wait();
    truncate_started.post();
    truncate_status = reader.Truncate(0);
    truncate_done.post();
  });
  fm.addTask([&] {
    truncate_started.wait();
    // Wait for a separate runnable fiber after Truncate has had its turn to
    // enter the read/write exclusion barrier.
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    truncate_probe_done.post();
  });
  const bool entered =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return get_started.try_wait() && truncate_probe_done.try_wait(); });
  EXPECT_TRUE(entered) << "truncate must reach the blocked read barrier before the GET is released";
  EXPECT_FALSE(truncate_done.try_wait()) << "truncate must not invalidate an in-flight read";
  EXPECT_EQ(meta_->MetadataMutationCalls(), 0) << "no size or attribute mutation may overtake the held GET";
  EXPECT_EQ(meta_->file_size(), 9);
  release_get.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return read_done.try_wait() && truncate_done.try_wait(); }, "Slice read/truncate completion");
  ASSERT_TRUE(read_status.ok()) << read_status.message();
  ASSERT_TRUE(truncate_status.ok()) << truncate_status.message();
  EXPECT_EQ(meta_->MetadataMutationCalls(), 1) << "the completed Shrink must reach its typed metadata path";
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), "persisted");
  EXPECT_EQ(meta_->file_size(), 0);
}

TEST_F(FileReadWriterSliceSizeTest, CompletedFlushCannotRegressConcurrentGetAttrSnapshot) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("published"), 0).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton inode_started;
  folly::fibers::Baton release_inode;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton attr_done;
  meta_->BlockNextGetInode(&inode_started, &release_inode);
  SwordFsInode inode;
  Status attr_status;
  Status flush_status;
  fm.addTask([&] {
    attr_status = writer.GetAttr(&inode);
    attr_done.post();
  });
  fm.addTask([&] {
    inode_started.wait();
    flush_status = writer.Flush();
    flush_done.post();
  });
  const bool completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return flush_done.try_wait(); });
  EXPECT_TRUE(completed) << "the Flush publication must not wait for an old GetAttr snapshot";
  release_inode.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && attr_done.try_wait(); }, "Slice GetAttr/Flush completion");
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_TRUE(attr_status.ok()) << attr_status.message();
  EXPECT_EQ(inode.attr.size, 9U);
  EXPECT_EQ(meta_->file_size(), 9);
}

TEST_F(FileReadWriterSliceSizeTest, StaleInodeSnapshotCannotRegressReadEofAfterCompletedFlush) {
  // Read must recheck the authoritative size epoch after its initial inode
  // lookup. The older GetInode result cannot turn a committed write into EOF.
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_inode_started;
  folly::fibers::Baton release_get_inode;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton read_done;
  meta_->BlockNextGetInode(&get_inode_started, &release_get_inode);

  Status read_status;
  Status write_status;
  Status flush_status;
  auto out = folly::IOBuf::create(13);
  fm.addTask([&] {
    read_status = writer.Read(13, 0, out.get());
    read_done.post();
  });
  fm.addTask([&] {
    get_inode_started.wait();
    write_status = writer.Write(Buf("Hello,_World!"), 0);
    if (write_status.ok()) {
      flush_status = writer.Flush();
    }
    flush_done.post();
  });

  const bool flushed = swordfs::test::DriveEventBaseUntil(evb, [&] { return flush_done.try_wait(); });
  EXPECT_TRUE(flushed) << "new Slice metadata must publish without waiting for a stale Read snapshot";
  if (!flushed) {
    release_get_inode.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && read_done.try_wait(); }, "Slice stale Read timeout cleanup");
    return;
  }
  EXPECT_TRUE(write_status.ok()) << write_status.message();
  EXPECT_TRUE(flush_status.ok()) << flush_status.message();
  EXPECT_EQ(meta_->file_size(), 13);

  release_get_inode.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return read_done.try_wait(); }, "Slice Read completes after stale inode snapshot release");
  ASSERT_TRUE(read_status.ok()) << read_status.message();
  EXPECT_EQ(out->length(), 13U);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello,_World!");
}

TEST_F(FileReadWriterSliceSizeTest, ConcurrentReadsRemainIndependentWhenFirstBackendGetIsBlocked) {
  RunInTestFiber([&] {
    FileReadWriter seed(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(seed.Write(Buf("persisted"), 0).ok());
    ASSERT_TRUE(seed.Flush().ok());
  });

  FileReadWriter cold(kIno, kMaxParallelFlushes);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_get_started;
  folly::fibers::Baton release_first_get;
  folly::fibers::Baton second_get_started;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  data_->BlockNextGet(&first_get_started, &release_first_get, &second_get_started);

  Status first_status;
  Status second_status;
  auto first_out = folly::IOBuf::create(9);
  auto second_out = folly::IOBuf::create(9);
  fm.addTask([&] {
    first_status = cold.Read(9, 0, first_out.get());
    first_done.post();
  });
  fm.addTask([&] {
    first_get_started.wait();
    second_status = cold.Read(9, 0, second_out.get());
    second_done.post();
  });

  const bool second_read_completed =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return second_get_started.try_wait() && second_done.try_wait(); });
  EXPECT_TRUE(second_read_completed) << "the second Slice read must not be serialized behind a blocked GET";
  if (!second_read_completed) {
    release_first_get.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return first_done.try_wait() && second_done.try_wait(); }, "Slice independent-read timeout cleanup");
    return;
  }
  // The first GET is still deliberately blocked. A failed assertion must
  // never return before the test releases that backend request.
  EXPECT_TRUE(second_status.ok()) << second_status.message();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second_out->data()), second_out->length()), "persisted");

  release_first_get.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_done.try_wait(); }, "Slice first read completion after GET release");
  ASSERT_TRUE(first_status.ok()) << first_status.message();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first_out->data()), first_out->length()), "persisted");
}

TEST_F(FileReadWriterSliceSizeTest, ConcurrentFlushesPublishGenerationsInOrder) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_put_started;
  folly::fibers::Baton release_first_put;
  folly::fibers::Baton write_done;
  folly::fibers::Baton second_flush_started;
  folly::fibers::Baton probe_done;
  folly::fibers::Baton first_flush_done;
  folly::fibers::Baton second_flush_done;
  data_->BlockNextPut(&first_put_started, &release_first_put);

  Status first_flush_status;
  Status second_flush_status;
  Status write_status;
  fm.addTask([&] {
    first_flush_status = writer.Flush();
    first_flush_done.post();
  });
  fm.addTask([&] {
    first_put_started.wait();
    write_status = writer.Write(Buf("BBBB"), 4);
    write_done.post();
  });
  fm.addTask([&] {
    write_done.wait();
    second_flush_started.post();
    second_flush_status = writer.Flush();
    second_flush_done.post();
  });
  fm.addTask([&] {
    second_flush_started.wait();
    // Wait until the second Flush has actually run or suspended, rather than
    // treating a pre-call baton as evidence of generation ordering.
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    probe_done.post();
  });

  const bool probe_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return probe_done.try_wait(); });
  EXPECT_TRUE(probe_completed) << "second Slice Flush must enter while the first PUT is held";
  if (!probe_completed) {
    release_first_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return first_flush_done.try_wait() && second_flush_done.try_wait(); },
        "Slice ordered-flush timeout cleanup");
    return;
  }
  EXPECT_EQ(data_->put_calls, 1) << "newer generation started uploading before the older Flush published";
  EXPECT_FALSE(second_flush_done.try_wait()) << "the later Flush must await the older generation";

  release_first_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_flush_done.try_wait() && second_flush_done.try_wait(); },
      "Slice ordered Flush completion after PUT release");
  ASSERT_TRUE(write_status.ok()) << write_status.message();
  ASSERT_TRUE(first_flush_status.ok()) << first_flush_status.message();
  ASSERT_TRUE(second_flush_status.ok()) << second_flush_status.message();
  EXPECT_EQ(data_->put_calls, 2);

  RunInTestFiber([&] {
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterSliceSizeTest, FailedOlderUploadPreservesWritesAcceptedDuringFlush) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton writes_done;
  folly::fibers::Baton flush_done;
  data_->put_status = Status::IOError("injected older Slice upload failure");
  data_->BlockNextPut(&put_started, &release_put);

  Status flush_status;
  Status first_write_status;
  Status second_write_status;
  fm.addTask([&] {
    flush_status = writer.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    first_write_status = writer.Write(Buf("BBBB"), 4);
    second_write_status = writer.Write(Buf("CC"), 1);
    writes_done.post();
  });

  const bool writes_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return writes_done.try_wait(); });
  EXPECT_TRUE(writes_completed) << "new Slice writes must not wait for the older failing upload";
  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && writes_done.try_wait(); }, "Slice failed upload completion cleanup");
  if (!writes_completed) {
    return;
  }
  ASSERT_TRUE(first_write_status.ok()) << first_write_status.message();
  ASSERT_TRUE(second_write_status.ok()) << second_write_status.message();
  EXPECT_EQ(flush_status.ToErrno(), EIO);

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(writer.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "ACCABBBB");

    data_->put_status = Status::OK();
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "ACCABBBB");
  });
}

TEST_F(FileReadWriterSliceSizeTest, FailedFirstMetadataAttachmentPreservesLaterWrites) {
  RunInTestFiber([&] {
    FileReadWriter writer(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(writer.Write(Buf("hello"), 0).ok());
    meta_->attach_prepared_status_ = Status::IOError("first Slice attachment failed before commit");
    EXPECT_EQ(writer.Flush().ToErrno(), EIO);

    swordfs::metadata::FileChunkSnapshot failed;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(kIno, 0, &failed).ok());
    EXPECT_FALSE(failed.chunk_id.has_value());
    EXPECT_EQ(failed.inode.attr.size, 0U);
    ASSERT_TRUE(writer.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(writer.Write(Buf("!"), 5).ok());

    meta_->attach_prepared_status_ = Status::OK();
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reader.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    EXPECT_EQ(published.inode.attr.size, 6U);
  });
}

TEST_F(FileReadWriterSliceSizeTest, WritesDuringAmbiguousFirstAttachmentPreserveLatestGeneration) {
  FileReadWriter writer(kIno, kMaxParallelFlushes);
  RunInTestFiber([&] { ASSERT_TRUE(writer.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton attach_started;
  folly::fibers::Baton release_attach;
  folly::fibers::Baton write_done;
  folly::fibers::Baton flush_done;
  meta_->attach_prepared_status_ = Status::OutcomeUnknown("reply lost after committed Slice attachment");
  meta_->attach_prepared_commit_on_error_ = true;
  meta_->BlockNextCommit(&attach_started, &release_attach);

  Status first_flush_status;
  Status write_status;
  fm.addTask([&] {
    first_flush_status = writer.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    attach_started.wait();
    write_status = writer.Write(Buf("BBBB"), 4);
    write_done.post();
  });

  const bool wrote = swordfs::test::DriveEventBaseUntil(evb, [&] { return write_done.try_wait(); });
  EXPECT_TRUE(wrote) << "writes accepted during an ambiguous Slice attach must not wait for its Redis reply";
  release_attach.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && write_done.try_wait(); }, "Slice ambiguous first-attach completion");
  if (!wrote) {
    return;
  }
  ASSERT_TRUE(write_status.ok()) << write_status.message();
  EXPECT_TRUE(first_flush_status.ok() || first_flush_status.IsOutcomeUnknown()) << first_flush_status.message();

  RunInTestFiber([&] {
    meta_->attach_prepared_status_ = Status::OK();
    meta_->attach_prepared_commit_on_error_ = false;
    ASSERT_TRUE(writer.Flush().ok());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(reader.Read(8, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "AAAABBBB");
    EXPECT_EQ(meta_->file_size(), 8);
  });
}
