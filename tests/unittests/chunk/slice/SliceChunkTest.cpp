// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Slice-specific Chunk tests, independent of COW metadata/fixture wiring.

#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "FiberTest.hpp"
#include "chunk/Chunk.hpp"
#include "chunk/ChunkFactory.hpp"
#include "chunk/ChunkTestSupport.hpp"
#include "chunk/slice/SliceRecord.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"
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
    *out = swordfs::chunk::slice::SliceID(1);
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

 private:
  std::map<uint64_t, std::vector<Record>> records_;
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

}  // namespace
