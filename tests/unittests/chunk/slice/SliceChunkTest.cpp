// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Slice-specific Chunk tests, independent of COW metadata/fixture wiring.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

#include "FiberTest.hpp"
#include "TestWatchdog.hpp"
#include "chunk/Chunk.hpp"
#include "chunk/ChunkFactory.hpp"
#include "chunk/ChunkTestSupport.hpp"
#include "chunk/slice/SliceChunk.hpp"
#include "chunk/slice/SliceRecord.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"

using swordfs::chunk::Chunk;
using swordfs::metadata::ChunkIndex;
using swordfs::test::RunInTestFiber;
using swordfs::test::chunk_test::Buf;
using swordfs::test::chunk_test::kChunkTestSize;
using swordfs::utils::Status;

namespace {

// Exercise the Slice implementation through the same mount-level factory API
// used by FileChunkManager; never inject visible spans through production hooks.
class TestSliceChunkMetadata final : public swordfs::metadata::slice::SliceChunkMetadata {
 public:
  using Record = swordfs::chunk::slice::SliceRecord;
  Status AllocateChunkID(swordfs::metadata::ChunkID *out) override {
    *out = swordfs::metadata::ChunkID(1);
    return Status::OK();
  }
  Status AllocateSliceID(swordfs::chunk::slice::SliceID *out) override {
    if (!allocation_status.ok()) {
      return allocation_status;
    }
    if (allocate_zero_id) {
      *out = swordfs::chunk::slice::SliceID();
      return Status::OK();
    }
    *out = swordfs::chunk::slice::SliceID(next_slice_id_++);
    return Status::OK();
  }
  Status CreateIndex(swordfs::metadata::ChunkID id, const Record &record) override {
    records_[id.Value()] = {record};
    return Status::OK();
  }
  Status ReadRecords(swordfs::metadata::ChunkID id, std::vector<Record> *out) override {
    if (read_error.has_value()) {
      return *read_error;
    }
    const auto it = records_.find(id.Value());
    if (it == records_.end()) {
      return Status::NotFound("missing Slice index");
    }
    *out = it->second;
    return Status::OK();
  }
  Status AppendRecord(swordfs::metadata::ChunkID id, const Record &record) override {
    records_[id.Value()].push_back(record);
    return Status::OK();
  }
  std::optional<Status> read_error;
  Status allocation_status = Status::OK();
  bool allocate_zero_id = false;

 private:
  std::map<uint64_t, std::vector<Record>> records_;
  uint64_t next_slice_id_ = 1000;
};

class SliceChunkTest : public swordfs::test::chunk_test::ChunkRuntimeTest {
 protected:
  void SetUp() override {
    InitializeLocalDoubles();
    slice_metadata_ = std::make_shared<TestSliceChunkMetadata>();
    factory_ = std::make_unique<swordfs::chunk::ChunkFactory>(swordfs::metadata::ChunkType::kChunkSlice,
                                                              slice_metadata_, meta_, data_, kChunkTestSize);
  }

  std::shared_ptr<Chunk> Open(bool create = true, ChunkIndex index = 0) {
    std::shared_ptr<Chunk> result;
    auto status = factory_->Open(42, index, create, &result);
    EXPECT_TRUE(status.ok()) << status.message();
    return result;
  }

  void Attach(ChunkIndex index, swordfs::metadata::ChunkID id, std::string_view payload,
              swordfs::chunk::slice::SliceID slice_id) {
    const auto start = index * kChunkTestSize;
    EXPECT_TRUE(slice_metadata_
                    ->CreateIndex(id, {0, slice_id, static_cast<uint32_t>(payload.size()), 0,
                                       static_cast<uint32_t>(payload.size())})
                    .ok());
    EXPECT_TRUE(data_->Put("slice/" + std::to_string(slice_id.Value()), folly::IOBuf::copyBuffer(payload)).ok());
    EXPECT_TRUE(
        meta_->AttachPrepared(42, index, id, start + payload.size(), {.eof = start, .boundary = std::nullopt}).ok());
  }

  std::shared_ptr<TestSliceChunkMetadata> slice_metadata_;
  std::unique_ptr<swordfs::chunk::ChunkFactory> factory_;
};

TEST_F(SliceChunkTest, PublishedSmallRangeUsesActualFactoryAndExactObjectOffsets) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(9), "abcdefgh", swordfs::chunk::slice::SliceID(11));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto out = folly::IOBuf::create(4);
    ASSERT_TRUE(chunk->Read(2, 4, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "cdef");
    EXPECT_EQ(data_->get_calls, 1);
    EXPECT_EQ(data_->requested_bytes, 4);
  });
}

TEST_F(SliceChunkTest, SparseLocalWriteOverlaysPublishedRangesWithoutHydration) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(5), "abcdefgh", swordfs::chunk::slice::SliceID(12));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(2, Buf("XY")).ok());
    ASSERT_TRUE(chunk->Write(3, Buf("Z")).ok());
    EXPECT_EQ(data_->get_calls, 0);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "abXZefgh");
    // Bytes covered by accepted dirty writes must not trigger remote GETs.
    EXPECT_EQ(data_->requested_bytes, 6);
    EXPECT_TRUE(chunk->HasPendingWrites());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, OrderedPublishedOverwritesAndZeroCoversWinByCommitOrder) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(4);
    Attach(0, id, "abcdefgh", SliceID(44));
    ASSERT_TRUE(data_->Put("slice/45", folly::IOBuf::copyBuffer("WXYZ")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {2, SliceID(45), 4, 0, 4}).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {3, SliceID(), 0, 0, 2}).ok());
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), std::string("abW\0\0Zgh", 8));
    EXPECT_GE(data_->get_calls, 2);  // Fragment coalescing is not part of the Reader contract.
    EXPECT_EQ(data_->requested_bytes, 6);
  });
}

TEST_F(SliceChunkTest, FailurePreservesOutputAndMissingMappedIndexIsNotSparse) {
  RunInTestFiber([&] {
    const swordfs::metadata::ChunkID id(3);
    Attach(0, id, "abcdefgh", swordfs::chunk::slice::SliceID(51));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto out = folly::IOBuf::create(16);
    std::memcpy(out->writableTail(), "OK", 2);
    out->append(2);
    slice_metadata_->read_error = Status::NotFound("missing mapped index");
    EXPECT_EQ(chunk->Read(0, 4, out.get()).ToErrno(), EIO);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "OK");
    slice_metadata_->read_error.reset();
    ASSERT_TRUE(data_->Put("slice/51", folly::IOBuf::copyBuffer("a")).ok());
    EXPECT_EQ(chunk->Read(0, 4, out.get()).ToErrno(), EIO);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "OK");
    EXPECT_EQ(chunk->Read(0, 15, out.get()).ToErrno(), EINVAL);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "OK");
  });
}

TEST_F(SliceChunkTest, DirtyHolesAndLocalTruncateNeverHydratePublishedData) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(5, Buf("ABCDE")).ok());
    ASSERT_TRUE(chunk->Write(7, Buf("! ")).ok());
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(chunk->Read(0, 11, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()),
              std::string("\0\0\0\0\0AB! E\0", 11));
    EXPECT_EQ(data_->get_calls, 0);
    chunk->TruncateLocal(8);
    auto after = folly::IOBuf::create(11);
    ASSERT_TRUE(chunk->Read(0, 11, after.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(after->data()), after->length()),
              std::string("\0\0\0\0\0AB!\0\0\0", 11));
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, FullyDirtyRangeReadsLocallyWithoutPhysicalGet) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(6), "abcdefgh", swordfs::chunk::slice::SliceID(15));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(0, Buf("12345678")).ok());
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "12345678");
    EXPECT_EQ(data_->get_calls, 0);
    EXPECT_EQ(data_->requested_bytes, 0);
  });
}

TEST_F(SliceChunkTest, UnmappedRangeAndInvalidInputsAreHandledWithoutRemoteHydration) {
  RunInTestFiber([&] {
    std::shared_ptr<Chunk> not_opened;
    ASSERT_TRUE(factory_->Open(42, 0, false, &not_opened).ok());
    EXPECT_EQ(not_opened, nullptr);
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto out = folly::IOBuf::create(16);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), std::string(8, '\0'));
    EXPECT_EQ(data_->get_calls, 0);
    EXPECT_FALSE(chunk->HasPendingWrites());
    EXPECT_EQ(chunk->Write(kChunkTestSize, Buf("x")).ToErrno(), EINVAL);
    EXPECT_EQ(chunk->Read(kChunkTestSize, 1, out.get()).ToErrno(), EINVAL);
    EXPECT_EQ(chunk->Read(0, 1, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(chunk->Read(0, 9, out.get()).ToErrno(), EINVAL);
    EXPECT_TRUE(chunk->Read(0, 0, out.get()).ok());
    EXPECT_FALSE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, CorruptPublishedHistoryAndFailedSnapshotAreHardErrors) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(21);
    Attach(0, id, "abcdefgh", SliceID(23));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto out = folly::IOBuf::create(8);
    meta_->file_snapshot_status = Status::IOError("snapshot failed");
    EXPECT_EQ(chunk->Read(0, 4, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0);
    meta_->file_snapshot_status = Status::OK();
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {kChunkTestSize - 1, SliceID(23), 8, 0, 2}).ok());
    EXPECT_EQ(chunk->Read(0, 4, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(SliceChunkTest, EmptyWriteDoesNotCreateDirtyStateAndBoundaryClampHidesPublishedTail) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(31), "abcdefgh", swordfs::chunk::slice::SliceID(32));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(chunk->Write(4, Buf("")).ok());
    EXPECT_FALSE(chunk->HasPendingWrites());

    // A retained boundary can hide stale published bytes without turning
    // them into writable dirty bytes or changing physical Slice metadata.
    chunk->ClampPublishedPrefix(4);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), out->length()), std::string("abcd\0\0\0\0", 8));
    EXPECT_EQ(data_->requested_bytes, 4);
    EXPECT_FALSE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, TransientPrivateMetadataFailurePreservesCallerOutput) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(41), "abcdefgh", swordfs::chunk::slice::SliceID(42));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    slice_metadata_->read_error = Status::IOError("private metadata temporarily unavailable");
    auto out = folly::IOBuf::create(8);
    EXPECT_EQ(chunk->Read(0, 8, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0);
    EXPECT_EQ(data_->get_calls, 0);
  });
}

TEST_F(SliceChunkTest, BoundaryClampTracksLaterCommittedHistoryForSameChunkId) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(63);
    Attach(0, id, "abcdefgh", SliceID(64));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto before = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, before.get()).ok());
    chunk->ClampPublishedPrefix(4);
    auto old_tail = folly::IOBuf::create(4);
    ASSERT_TRUE(chunk->Read(4, 4, old_tail.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(old_tail->data()), 4), std::string(4, '\0'));
    ASSERT_TRUE(data_->Put("slice/65", folly::IOBuf::copyBuffer("WXYZ")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {4, SliceID(65), 4, 0, 4}).ok());
    auto fresh_tail = folly::IOBuf::create(4);
    ASSERT_TRUE(chunk->Read(4, 4, fresh_tail.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(fresh_tail->data()), 4), "WXYZ");
    auto reopened = Open();
    ASSERT_NE(reopened, nullptr);
    auto remounted_tail = folly::IOBuf::create(4);
    ASSERT_TRUE(reopened->Read(4, 4, remounted_tail.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(remounted_tail->data()), 4), "WXYZ");
    auto mixed = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, mixed.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(mixed->data()), 8), "abcdWXYZ");
  });
}

TEST_F(SliceChunkTest, BoundaryClampDoesNotMaskReplacedAuthoritativeChunkId) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    Attach(0, swordfs::metadata::ChunkID(71), "abcdefgh", SliceID(72));
    auto cached = Open();
    ASSERT_NE(cached, nullptr);
    cached->ClampPublishedPrefix(4);
    ASSERT_TRUE(slice_metadata_->CreateIndex(swordfs::metadata::ChunkID(73), {0, SliceID(74), 8, 0, 8}).ok());
    ASSERT_TRUE(data_->Put("slice/74", folly::IOBuf::copyBuffer("12345678")).ok());
    meta_->ReplaceAttachment(0, swordfs::metadata::ChunkID(73), 8);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(cached->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), 8), "12345678");
  });
}

TEST_F(SliceChunkTest, LaterPhysicalGetFailureLeavesEarlierCompletedSpanUnpublished) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(81);
    Attach(0, id, "abcdefgh", SliceID(82));
    ASSERT_TRUE(data_->Put("slice/83", folly::IOBuf::copyBuffer("WXYZ")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {2, SliceID(83), 4, 0, 4}).ok());
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    data_->fail_get_call = 2;
    auto out = folly::IOBuf::create(12);
    std::memcpy(out->writableTail(), "OK", 2);
    out->append(2);
    EXPECT_EQ(chunk->Read(0, 8, out.get()).ToErrno(), EIO);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "OK");
    EXPECT_EQ(data_->get_calls, 2);
  });
}

TEST_F(SliceChunkTest, HistoricalSameSliceIdCanGrowWithNonzeroObjectOffset) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(91);
    Attach(0, id, "abcdefgh", SliceID(92));
    ASSERT_TRUE(data_->Put("slice/92", folly::IOBuf::copyBuffer("abcdefghijkl")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {4, SliceID(92), 12, 8, 4}).ok());
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(2, 6, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), 6), "cdijkl");
    EXPECT_EQ(data_->requested_bytes, 6);
  });
}

TEST_F(SliceChunkTest, OldTruncatedSuffixStaysHiddenUntilCoveredByNewCommit) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(101);
    Attach(0, id, "abcdefgh", SliceID(102));
    ASSERT_TRUE(data_->Put("slice/103", folly::IOBuf::copyBuffer("XY")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {4, SliceID(103), 2, 0, 2}).ok());
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    chunk->ClampPublishedPrefix(4);
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {1, SliceID(103), 2, 0, 2}).ok());
    auto still_hidden = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, still_hidden.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(still_hidden->data()), 8), std::string("aXYd\0\0\0\0", 8));
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {6, SliceID(103), 2, 0, 2}).ok());
    auto after_cover = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, after_cover.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(after_cover->data()), 8), std::string("aXYd\0\0XY", 8));
  });
}

TEST_F(SliceChunkTest, BoundaryProvenanceFailureIsFailClosedAndCanBeRecaptured) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(111), "abcdefgh", swordfs::chunk::slice::SliceID(112));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    meta_->file_snapshot_status = Status::IOError("cannot capture sanitation identity");
    chunk->ClampPublishedPrefix(4);
    meta_->file_snapshot_status = Status::OK();
    auto out = folly::IOBuf::create(8);
    EXPECT_EQ(chunk->Read(0, 8, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0);
    chunk->ClampPublishedPrefix(4);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), 8), std::string("abcd\0\0\0\0", 8));
  });
}

TEST_F(SliceChunkTest, RepeatedClampKeepsNewlyPublishedPrefixAndNeverRevivesOldTail) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(121);
    Attach(0, id, "abcdefgh", SliceID(122));
    auto cached = Open();
    ASSERT_NE(cached, nullptr);

    // Old R0 tail [4,8) is no longer visible after the first shrink.
    meta_->ReplaceAttachment(0, id, 4);
    cached->ClampPublishedPrefix(4);
    // The same ChunkID gains a newly committed covering R1 after growth.
    ASSERT_TRUE(data_->Put("slice/123", folly::IOBuf::copyBuffer("WXYZ")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {4, SliceID(123), 4, 0, 4}).ok());
    meta_->ReplaceAttachment(0, id, 8);
    auto published = folly::IOBuf::create(4);
    ASSERT_TRUE(cached->Read(4, 4, published.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(published->data()), 4), "WXYZ");

    // Shrink again, now keeping [4,6). The earlier clamp must not be
    // applied retroactively to R1, but this clamp must hide R1's [6,8).
    meta_->ReplaceAttachment(0, id, 6);
    cached->ClampPublishedPrefix(6);
    auto surviving = folly::IOBuf::create(2);
    ASSERT_TRUE(cached->Read(4, 2, surviving.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(surviving->data()), 2), "WX");
    auto outside_eof = folly::IOBuf::create(2);
    ASSERT_TRUE(cached->Read(6, 2, outside_eof.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(outside_eof->data()), 2), std::string(2, '\0'));

    // Extending FileMetadata alone must never resurrect R0 or R1's old tail.
    meta_->ReplaceAttachment(0, id, 8);
    auto after_grow = folly::IOBuf::create(8);
    ASSERT_TRUE(cached->Read(0, 8, after_grow.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(after_grow->data()), 8), std::string("abcdWX\0\0", 8));

    // Only a third, freshly committed record can make the hidden tail visible.
    ASSERT_TRUE(data_->Put("slice/124", folly::IOBuf::copyBuffer("YZ")).ok());
    ASSERT_TRUE(slice_metadata_->AppendRecord(id, {6, SliceID(124), 2, 0, 2}).ok());
    auto refreshed = folly::IOBuf::create(8);
    ASSERT_TRUE(cached->Read(0, 8, refreshed.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(refreshed->data()), 8), "abcdWXYZ");
  });
}

TEST_F(SliceChunkTest, RepeatedClampWithoutNewRecordsUsesStrictestVisiblePrefix) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(131), "abcdefgh", swordfs::chunk::slice::SliceID(132));
    auto cached = Open();
    ASSERT_NE(cached, nullptr);
    cached->ClampPublishedPrefix(6);
    cached->ClampPublishedPrefix(4);
    // A later redundant, weaker sanitation cannot restore the old tail.
    cached->ClampPublishedPrefix(7);
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(cached->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), 8), std::string("abcd\0\0\0\0", 8));
  });
}

// #485 TDD: the normal Chunk API, real Slice factory, and immutable data
// engine form the public contract. No metadata publication is permitted until
// the separate #271 atomic file-commit protocol is available.
TEST_F(SliceChunkTest, FlushFreezesAdjacentWritesIntoOneImmutableUnpublishedObject) {
  RunInTestFiber([&] {
    using swordfs::chunk::slice::SliceID;
    const swordfs::metadata::ChunkID id(200);
    Attach(0, id, "abcdefgh", SliceID(900));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    const int previous_puts = data_->put_calls;
    ASSERT_TRUE(chunk->Write(2, Buf("WX")).ok());
    ASSERT_TRUE(chunk->Write(4, Buf("YZ")).ok());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    ASSERT_EQ(data_->put_calls, previous_puts + 1);
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1000");
    auto physical = folly::IOBuf::create(4);
    ASSERT_TRUE(data_->Get("slice/1000", 0, 4, physical.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(physical->data()), 4), "WXYZ");
    std::vector<swordfs::chunk::slice::SliceRecord> committed;
    ASSERT_TRUE(slice_metadata_->ReadRecords(id, &committed).ok());
    EXPECT_EQ(committed.size(), 1U) << "Put must not publish an uncommitted SliceRecord";
    EXPECT_TRUE(chunk->HasPendingWrites());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, previous_puts + 1) << "prepared generations must not PUT again";
  });
}

TEST_F(SliceChunkTest, SparseFreezeDoesNotManufactureZeroCoversAcrossUnwrittenGaps) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(201), "abcdefgh", swordfs::chunk::slice::SliceID(901));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    const int previous_puts = data_->put_calls;
    ASSERT_TRUE(chunk->Write(1, Buf("X")).ok());
    ASSERT_TRUE(chunk->Write(5, Buf("Y")).ok());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    ASSERT_EQ(data_->put_calls, previous_puts + 2);
    EXPECT_EQ(data_->uploaded_keys[data_->uploaded_keys.size() - 2], "slice/1000");
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1001");
    auto first = folly::IOBuf::create(1);
    auto second = folly::IOBuf::create(1);
    ASSERT_TRUE(data_->Get("slice/1000", 0, 1, first.get()).ok());
    ASSERT_TRUE(data_->Get("slice/1001", 0, 1, second.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first->data()), 1), "X");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second->data()), 1), "Y");
    auto out = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), 8), "aXcdeYgh");
  });
}

TEST_F(SliceChunkTest, FreezePreservesOlderDataWhileNewWriteOverlapsInFlightPut) {
  RunInTestFiber([&] { Attach(0, swordfs::metadata::ChunkID(202), "abcdefgh", swordfs::chunk::slice::SliceID(902)); });
  auto chunk = Open();
  ASSERT_NE(chunk, nullptr);
  RunInTestFiber([&] { ASSERT_TRUE(chunk->Write(2, Buf("WXYZ")).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton flush_done;
  data_->BlockNextPut(&put_started, &release_put);
  Status flush_status;
  fm.addTask([&] {
    flush_status = chunk->Flush();
    flush_done.post();
  });
  // The pre-implementation RED must fail without leaving a fiber blocked
  // forever: the old NotSupported Flush never enters IDataEngine::Put.
  const bool started = swordfs::test::DriveEventBaseUntil(evb, [&] { return put_started.try_wait(); });
  if (!started) {
    EXPECT_TRUE(started) << "Slice freeze must enter immutable Put";
    return;
  }
  fm.addTask([&] {
    EXPECT_TRUE(chunk->HasPendingWrites());
    EXPECT_TRUE(chunk->Write(4, Buf("12")).ok());
    auto during = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, during.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(during->data()), 8), "abWX12gh");
    release_put.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(evb, [&] { return flush_done.try_wait(); }, "Slice freeze completion");
  EXPECT_EQ(flush_status.ToErrno(), ENOSYS);
  RunInTestFiber([&] {
    auto after = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, after.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(after->data()), 8), "abWX12gh");
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    ASSERT_EQ(data_->uploaded_keys.back(), "slice/1001");
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, FailedOrAmbiguousPutKeepsDirtyBytesAndRetriesWithFreshSliceIdentity) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(2, Buf("WXYZ")).ok());
    data_->next_put_status = Status::OutcomeUnknown("lost PUT response");
    EXPECT_FALSE(chunk->Flush().ok());
    ASSERT_FALSE(data_->uploaded_keys.empty());
    ASSERT_EQ(data_->uploaded_keys.back(), "slice/1000");
    EXPECT_TRUE(chunk->HasPendingWrites());
    ASSERT_TRUE(chunk->Write(4, Buf("12")).ok());
    auto before_retry = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, before_retry.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(before_retry->data()), 8), std::string("\0\0WX12\0\0", 8));
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    ASSERT_GE(data_->uploaded_keys.size(), 3U);
    EXPECT_EQ(data_->uploaded_keys[data_->uploaded_keys.size() - 2], "slice/1001");
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1002");
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, LocalTruncateClipsFrozenVisibilityAndNeverReuploadsPreparedData) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(2, Buf("WXYZ")).ok());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 1);
    chunk->TruncateLocal(4);
    auto result = folly::IOBuf::create(8);
    ASSERT_TRUE(chunk->Read(0, 8, result.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(result->data()), 8), std::string("\0\0WX\0\0\0\0", 8));
    EXPECT_TRUE(chunk->HasPendingWrites());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 1);
  });
}

TEST_F(SliceChunkTest, RepeatedFlushWhilePhysicalPutIsInFlightReportsBusyWithoutDuplicatePut) {
  auto chunk = Open();
  ASSERT_NE(chunk, nullptr);
  RunInTestFiber([&] { ASSERT_TRUE(chunk->Write(0, Buf("abcd")).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton started;
  folly::fibers::Baton release;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  data_->BlockNextPut(&started, &release);
  Status first_status;
  Status second_status;
  fm.addTask([&] {
    first_status = chunk->Flush();
    first_done.post();
  });
  const bool uploaded = swordfs::test::DriveEventBaseUntil(evb, [&] { return started.try_wait(); });
  if (!uploaded) {
    EXPECT_TRUE(uploaded) << "first Flush never started Put";
    return;
  }
  fm.addTask([&] {
    EXPECT_TRUE(chunk->HasPendingWrites());
    second_status = chunk->Flush();
    second_done.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(evb, [&] { return second_done.try_wait(); }, "concurrent Slice Flush");
  EXPECT_EQ(second_status.ToErrno(), EBUSY);
  release.post();
  swordfs::test::DriveEventBaseUntilOrAbort(evb, [&] { return first_done.try_wait(); }, "first Slice Flush");
  EXPECT_EQ(first_status.ToErrno(), ENOSYS);
  RunInTestFiber([&] {
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
  EXPECT_EQ(data_->put_calls, 1);
}

TEST_F(SliceChunkTest, AllocationFailureAndBadSliceIdentityNeverPutOrPublish) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(3, Buf("ABC")).ok());
    slice_metadata_->allocation_status = Status::IOError("cannot allocate SliceID");
    EXPECT_EQ(chunk->Flush().ToErrno(), EIO);
    EXPECT_EQ(data_->put_calls, 0);
    slice_metadata_->allocation_status = Status::OK();
    slice_metadata_->allocate_zero_id = true;
    EXPECT_FALSE(chunk->Flush().ok());
    EXPECT_EQ(data_->put_calls, 0);
    slice_metadata_->allocate_zero_id = false;
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 1);
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1000");
    auto out = folly::IOBuf::create(3);
    ASSERT_TRUE(chunk->Read(3, 3, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), 3), "ABC");
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, FailedDefinitePutUsesFreshKeyOnRetryWithoutRemovingLocalDirtyData) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(1, Buf("PQ")).ok());
    data_->next_put_status = Status::IOError("definite upload failure");
    EXPECT_EQ(chunk->Flush().ToErrno(), EIO);
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1000");
    auto first_read = folly::IOBuf::create(2);
    ASSERT_TRUE(chunk->Read(1, 2, first_read.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first_read->data()), 2), "PQ");
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1001");
    EXPECT_EQ(data_->put_calls, 2);
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, FrozenDirtyViewStillSkipsPhysicalGetsAndPreservesOutputOnLateGetError) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(208), "abcdefgh", swordfs::chunk::slice::SliceID(908));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(2, Buf("WXYZ")).ok());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    const int initial_gets = data_->get_calls;
    auto dirty_only = folly::IOBuf::create(4);
    ASSERT_TRUE(chunk->Read(2, 4, dirty_only.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(dirty_only->data()), 4), "WXYZ");
    EXPECT_EQ(data_->get_calls, initial_gets);
    data_->fail_get_call = initial_gets + 2;
    auto full = folly::IOBuf::create(10);
    std::memcpy(full->writableTail(), "OK", 2);
    full->append(2);
    EXPECT_EQ(chunk->Read(0, 8, full.get()).ToErrno(), EIO);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(full->data()), 2), "OK");
    EXPECT_EQ(full->length(), 2U);
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, LargeAcceptedWriteAutoFreezesIntoBoundedImmutableObjects) {
  RunInTestFiber([&] {
    constexpr size_t kLargeChunkSize = 8 * 1024 * 1024;
    constexpr size_t kSliceSize = 4 * 1024 * 1024;
    swordfs::chunk::slice::SliceChunk chunk(42, 0, kLargeChunkSize, slice_metadata_, meta_, data_);
    std::string payload(kLargeChunkSize, 'a');
    ASSERT_TRUE(chunk.Write(0, Buf(payload)).ok());
    // Auto-freeze is local preparation only, never premature data publication.
    EXPECT_EQ(data_->put_calls, 0);
    ASSERT_TRUE(chunk.Write(kSliceSize - 2, Buf("WXYZ")).ok());
    auto read = folly::IOBuf::create(4);
    ASSERT_TRUE(chunk.Read(kSliceSize - 2, 4, read.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(read->data()), 4), "WXYZ");
    EXPECT_EQ(chunk.Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 3);
    EXPECT_EQ(data_->uploaded_keys[0], "slice/1000");
    EXPECT_EQ(data_->uploaded_keys[1], "slice/1001");
    EXPECT_EQ(data_->uploaded_keys[2], "slice/1002");
    EXPECT_TRUE(chunk.HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, ContiguousSmallWritesCoalesceWithoutOnePutPerSyscall) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    for (size_t offset = 0; offset < kChunkTestSize; offset += 4) {
      ASSERT_TRUE(chunk->Write(offset, Buf("abcd")).ok());
    }
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 1);
    auto physical = folly::IOBuf::create(kChunkTestSize);
    ASSERT_TRUE(data_->Get("slice/1000", 0, kChunkTestSize, physical.get()).ok());
    std::string expected;
    expected.reserve(kChunkTestSize);
    for (size_t i = 0; i < kChunkTestSize / 4; ++i) {
      expected += "abcd";
    }
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(physical->data()), physical->length()), expected);
  });
}

TEST_F(SliceChunkTest, OverlappingLargeWriteDoesNotGrowMutableSlicePastFreezeLimit) {
  RunInTestFiber([&] {
    constexpr size_t kMiB = 1024 * 1024;
    swordfs::chunk::slice::SliceChunk chunk(42, 0, 8 * kMiB, slice_metadata_, meta_, data_);
    ASSERT_TRUE(chunk.Write(0, Buf(std::string(3 * kMiB, 'a'))).ok());
    ASSERT_TRUE(chunk.Write(2 * kMiB, Buf(std::string(4 * kMiB, 'b'))).ok());
    EXPECT_EQ(chunk.Flush().ToErrno(), ENOSYS);
    ASSERT_EQ(data_->put_calls, 2);
    // Freezing before a too-large union keeps each immutable candidate
    // bounded, and a later overlapping generation wins locally.
    auto a = folly::IOBuf::create(3 * kMiB + 1);
    auto b = folly::IOBuf::create(4 * kMiB + 1);
    ASSERT_TRUE(data_->Get("slice/1000", 0, 3 * kMiB + 1, a.get()).ok());
    ASSERT_TRUE(data_->Get("slice/1001", 0, 4 * kMiB + 1, b.get()).ok());
    EXPECT_EQ(a->length(), 3 * kMiB);
    EXPECT_EQ(b->length(), 4 * kMiB);
    auto visible = folly::IOBuf::create(4);
    ASSERT_TRUE(chunk.Read(2 * kMiB - 2, 4, visible.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(visible->data()), 4), "aabb");
  });
}

TEST_F(SliceChunkTest, AutoFreezeDuringOlderInFlightPutKeepsCandidateLifetimeAndNewerDirtyVisible) {
  constexpr size_t kLargeChunkSize = 8 * 1024 * 1024;
  swordfs::chunk::slice::SliceChunk chunk(42, 0, kLargeChunkSize, slice_metadata_, meta_, data_);
  RunInTestFiber([&] { ASSERT_TRUE(chunk.Write(0, Buf("abcd")).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton started;
  folly::fibers::Baton release;
  folly::fibers::Baton done;
  Status flush_status;
  data_->BlockNextPut(&started, &release);
  fm.addTask([&] {
    flush_status = chunk.Flush();
    done.post();
  });
  const bool uploaded = swordfs::test::DriveEventBaseUntil(evb, [&] { return started.try_wait(); });
  if (!uploaded) {
    EXPECT_TRUE(uploaded) << "first frozen object never started Put";
    return;
  }
  fm.addTask([&] {
    std::string later(kLargeChunkSize, 'z');
    EXPECT_TRUE(chunk.Write(0, Buf(later)).ok());
    auto dirty = folly::IOBuf::create(4);
    EXPECT_TRUE(chunk.Read(0, 4, dirty.get()).ok());
    if (dirty->length() == 4) {
      EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(dirty->data()), 4), "zzzz");
    }
    release.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(evb, [&] { return done.try_wait(); }, "Slice auto-freeze and older Put");
  EXPECT_EQ(flush_status.ToErrno(), ENOSYS);
  RunInTestFiber([&] {
    EXPECT_EQ(chunk.Flush().ToErrno(), ENOSYS);
    EXPECT_TRUE(chunk.HasPendingWrites());
  });
  EXPECT_EQ(data_->put_calls, 3);
}

TEST_F(SliceChunkTest, FlushRejectsUnrepresentableChunkRangeBeforePhysicalSideEffects) {
  RunInTestFiber([&] {
    const size_t beyond_record_range = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1;
    swordfs::chunk::slice::SliceChunk chunk(42, 0, beyond_record_range, slice_metadata_, meta_, data_);
    ASSERT_TRUE(chunk.Write(0, Buf("X")).ok());
    EXPECT_EQ(chunk.Flush().ToErrno(), EINVAL);
    EXPECT_EQ(data_->put_calls, 0);
    EXPECT_TRUE(chunk.HasPendingWrites());
  });
}

TEST_F(SliceChunkTest, SparseRewritesPreserveGapsAndRemainTwoIndependentObjects) {
  RunInTestFiber([&] {
    Attach(0, swordfs::metadata::ChunkID(309), "abcdefghij", swordfs::chunk::slice::SliceID(909));
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(1, Buf("X")).ok());
    ASSERT_TRUE(chunk->Write(5, Buf("Y")).ok());
    ASSERT_TRUE(chunk->Write(5, Buf("Q")).ok());
    ASSERT_TRUE(chunk->Write(0, Buf("A")).ok());
    ASSERT_TRUE(chunk->Write(6, Buf("Z")).ok());
    ASSERT_TRUE(chunk->Write(0, Buf("B")).ok());
    auto actual = folly::IOBuf::create(10);
    ASSERT_TRUE(chunk->Read(0, 10, actual.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(actual->data()), 10), "BXcdeQZhij");
    const int puts_before = data_->put_calls;
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, puts_before + 2);
    auto left = folly::IOBuf::create(2);
    auto right = folly::IOBuf::create(2);
    ASSERT_TRUE(data_->Get("slice/1000", 0, 2, left.get()).ok());
    ASSERT_TRUE(data_->Get("slice/1001", 0, 2, right.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(left->data()), 2), "BX");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(right->data()), 2), "QZ");
  });
}

TEST_F(SliceChunkTest, TruncatingFullyPreparedFrozenTailDoesNotPublishOrReuploadIt) {
  RunInTestFiber([&] {
    auto chunk = Open();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->Write(8, Buf("ZZ")).ok());
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    ASSERT_EQ(data_->put_calls, 1);
    chunk->TruncateLocal(4);
    EXPECT_FALSE(chunk->HasPendingWrites());
    auto output = folly::IOBuf::create(10);
    ASSERT_TRUE(chunk->Read(0, 10, output.get()).ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(output->data()), 10), std::string(10, '\0'));
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 1) << "retired prepared bytes must not be reuploaded";
    ASSERT_TRUE(chunk->Write(6, Buf("A")).ok());
    chunk->TruncateLocal(10);
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(data_->put_calls, 2);
    EXPECT_EQ(data_->uploaded_keys.back(), "slice/1001");
  });
}

TEST_F(SliceChunkTest, OverlappingWriteUpToFourMiBUsesOneFrozenObject) {
  RunInTestFiber([&] {
    constexpr size_t kMiB = 1024 * 1024;
    swordfs::chunk::slice::SliceChunk chunk(42, 0, 8 * kMiB, slice_metadata_, meta_, data_);
    ASSERT_TRUE(chunk.Write(0, Buf(std::string(3 * kMiB, 'a'))).ok());
    ASSERT_TRUE(chunk.Write(2 * kMiB, Buf(std::string(2 * kMiB, 'b'))).ok());
    EXPECT_EQ(chunk.Flush().ToErrno(), ENOSYS);
    ASSERT_EQ(data_->put_calls, 1);
    auto physical = folly::IOBuf::create(4 * kMiB);
    ASSERT_TRUE(data_->Get("slice/1000", 0, 4 * kMiB, physical.get()).ok());
    ASSERT_EQ(physical->length(), 4 * kMiB);
    EXPECT_EQ(physical->data()[0], 'a');
    EXPECT_EQ(physical->data()[2 * kMiB - 1], 'a');
    EXPECT_EQ(physical->data()[2 * kMiB], 'b');
    EXPECT_EQ(physical->data()[4 * kMiB - 1], 'b');
  });
}

// A sink backend isolates the writer's local payload ownership from remote
// object persistence, which must not count as cached memory on this mount.
class DiscardingSliceDataEngine final : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    ++put_calls;
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::NotFound("sink has no objects");
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }
  size_t put_calls = 0;
};

#ifdef __linux__
// statm's resident field counts pages currently in RAM, unlike ru_maxrss's
// lifetime high-water mark, which can mask a leak after earlier large tests.
std::optional<size_t> CurrentResidentBytes() {
  std::ifstream statm("/proc/self/statm");
  size_t virtual_pages = 0;
  size_t resident_pages = 0;
  const long page_bytes = sysconf(_SC_PAGESIZE);
  if (page_bytes <= 0 || !(statm >> virtual_pages >> resident_pages)) {
    return std::nullopt;
  }
  return resident_pages * static_cast<size_t>(page_bytes);
}
#endif

TEST_F(SliceChunkTest, RepeatedNonzeroEofTruncateRetiresAllInvisibleFrozenPayloads) {
#ifdef __linux__
  constexpr size_t kMiB = 1024 * 1024;
  constexpr size_t kRounds = 50;
  constexpr size_t kWarmupRounds = 4;
  constexpr size_t kPayloadBytes = 4 * kMiB;
  // Permits allocator arena reuse, Folly bookkeeping and unrelated page
  // residency fluctuations of up to eight payloads. A retained frozen
  // payload per round would instead accumulate 50 * 4 MiB = 200 MiB.
  constexpr size_t kAllowedRssGrowth = 8 * kPayloadBytes;
  DiscardingSliceDataEngine sink;
  RunInTestFiber([&] {
    swordfs::chunk::slice::SliceChunk chunk(42, 0, 8 * kMiB, slice_metadata_, meta_, &sink);
    std::string payload(kPayloadBytes, 'x');
    const auto write_freeze_truncate = [&] {
      ASSERT_TRUE(chunk.Write(1, Buf(payload)).ok());
      EXPECT_EQ(chunk.Flush().ToErrno(), ENOSYS);
      chunk.TruncateLocal(1);
      EXPECT_FALSE(chunk.HasPendingWrites());
    };
    for (size_t i = 0; i < kWarmupRounds; ++i) {
      write_freeze_truncate();
    }
    const auto baseline = CurrentResidentBytes();
    ASSERT_TRUE(baseline.has_value()) << "cannot read current resident pages from /proc/self/statm";
    for (size_t i = 0; i < kRounds; ++i) {
      write_freeze_truncate();
    }
    const auto retained = CurrentResidentBytes();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(chunk.Flush().ToErrno(), ENOSYS);
    EXPECT_EQ(sink.put_calls, kWarmupRounds + kRounds);
    EXPECT_LE(*retained, *baseline + kAllowedRssGrowth)
        << "repeatedly retired frozen payloads must not accumulate resident memory";
  });
#else
  GTEST_SKIP() << "the Linux-specific RSS bound is verified by GitHub CI";
#endif
}

TEST_F(SliceChunkTest, TruncateWhilePutInFlightRetiresOldCandidateWithoutLosingNewWrite) {
  auto chunk = Open();
  ASSERT_NE(chunk, nullptr);
  RunInTestFiber([&] { ASSERT_TRUE(chunk->Write(8, Buf("OLD")).ok()); });
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton flush_done;
  Status result;
  data_->BlockNextPut(&put_started, &release_put);
  fm.addTask([&] {
    result = chunk->Flush();
    flush_done.post();
  });
  ASSERT_TRUE(swordfs::test::DriveEventBaseUntil(evb, [&] { return put_started.try_wait(); }));
  fm.addTask([&] {
    chunk->TruncateLocal(1);
    EXPECT_FALSE(chunk->HasPendingWrites());
    EXPECT_TRUE(chunk->Write(8, Buf("NEW")).ok());
    release_put.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(evb, [&] { return flush_done.try_wait(); }, "Slice in-flight retirement");
  EXPECT_EQ(result.ToErrno(), ENOSYS);
  RunInTestFiber([&] {
    auto out = folly::IOBuf::create(3);
    ASSERT_TRUE(chunk->Read(8, 3, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), 3), "NEW");
    EXPECT_EQ(chunk->Flush().ToErrno(), ENOSYS);
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
  ASSERT_EQ(data_->put_calls, 2);
  EXPECT_EQ(data_->uploaded_keys.back(), "slice/1001");
}

}  // namespace
