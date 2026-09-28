// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for Chunk — focused on the invariant between
// `max_chunk_size_` (the chunk's notion of its own size, used to
// compute StartOffset) and the write buffer's capacity. If those
// drift, writes that cross what the buffer thinks is "beyond capacity"
// but stay within what the chunk thinks is "in range" return EINVAL
// even though the chunk is in kDirty state.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "FiberTest.hpp"
#include "TestWatchdog.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/Chunk.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/IChunkOverwriteStrategy.hpp"
#include "chunk/WriteBuf.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::chunk::Chunk;
using swordfs::metadata::ChunkIndex;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::storage::IDataEngine;
using swordfs::test::RunInTestFiber;
using swordfs::utils::Status;

namespace {

constexpr uint64_t kChunkTestSize = 1024;

class NoopPrivateMetadataStore final : public swordfs::metadata::IChunkPrivateMetadataStore {
 public:
  Status AllocateSequence(swordfs::metadata::ChunkPrivateSequenceKey, uint64_t *value) override {
    if (value == nullptr) {
      return Status::InvalidArgument("private sequence output is null");
    }
    *value = 1;
    return Status::OK();
  }
};

auto Buf(const std::string &value) {
  return *folly::IOBuf::copyBuffer(value.data(), value.size());
}

// Minimal meta engine: FindChunk always returns NotFound so the chunk
// transitions to kDirty and allocates a write buffer.
class MissingMetaEngine : public IMetaEngine {
 public:
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
  Status FindChunk(InodeID, ChunkIndex, SwordFsChunk *) override {
    return Status::NotFound("no chunk");
  }
  // Everything else is irrelevant for these tests.
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInodes(const std::vector<InodeID> &, std::vector<std::optional<SwordFsInode>> *) override {
    return Status::NotSupported("batch inode lookup is not used by this test double");
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
  Status Unlink(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) override {
    return Status::OK();
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
  Status Open(InodeID, uint64_t *size = nullptr) override {
    if (size != nullptr) {
      *size = 0;
    }
    return Status::OK();
  }
  Status PrepareReclaim(InodeID, std::optional<swordfs::metadata::ReclaimWork> *work) override {
    work->reset();
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
  Status VisitPendingDeletesBatch(size_t, const swordfs::metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }
  Status CompletePendingDelete(std::string_view) override {
    return Status::OK();
  }
  Status AllocateChunkRevision(swordfs::metadata::ChunkRevision *revision) override {
    if (revision == nullptr) {
      return Status::InvalidArgument("chunk revision output is null");
    }
    *revision = next_revision_++;
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status CommitChunk(InodeID, const std::optional<SwordFsChunk> &, const SwordFsChunk &) override {
    return Status::OK();
  }
  Status Truncate(InodeID, uint64_t) override {
    return Status::OK();
  }

 private:
  swordfs::metadata::ChunkRevision next_revision_ = 1;
};

// Minimal data engine: nothing is actually persisted; the chunk only
// calls Put on Flush, and these tests don't reach that point.
class NullDataEngine final : public IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    ++put_calls;
    if (put_started_ != nullptr) {
      auto *started = put_started_;
      auto *release = put_release_;
      put_started_ = nullptr;
      put_release_ = nullptr;
      started->post();
      release->wait();
    }
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::NotFound("nope");
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }

  void BlockNextPut(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    put_started_ = started;
    put_release_ = release;
  }

  int put_calls = 0;

 private:
  folly::fibers::Baton *put_started_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
};

NullDataEngine *InitializeRuntime() {
  SwordFsVolume config;
  config.chunk_size = kChunkTestSize;
  auto data = std::make_unique<NullDataEngine>();
  auto *raw = data.get();
  auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<MissingMetaEngine>>();
  const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
  EXPECT_TRUE(status.ok()) << status.message();
  return raw;
}

}  // namespace

class ChunkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    data_ = InitializeRuntime();
  }

  NullDataEngine *data_ = nullptr;
};

TEST(ChunkObjectKeyTest, IncludesInodeIndexAndRevision) {
  EXPECT_EQ(swordfs::chunk::FormatChunkObjectKey(/*ino=*/42, /*index=*/3, /*revision=*/7), "42/3/7");
}

TEST(SwordFsChunkDescriptorTest, ValidatesCanonicalFixedSizeIdentity) {
  constexpr uint64_t kChunkSize = 64;
  const SwordFsChunk valid{.index = 1, .revision = 1, .size = kChunkSize};
  EXPECT_TRUE(valid.IsValidForChunkSize(kChunkSize));

  auto invalid = valid;
  EXPECT_FALSE(invalid.IsValidForChunkSize(0));

  invalid = valid;
  invalid.revision = swordfs::metadata::kInvalidChunkRevision;
  EXPECT_FALSE(invalid.IsValidForChunkSize(kChunkSize));

  invalid = valid;
  invalid.size = kChunkSize + 1;
  EXPECT_FALSE(invalid.IsValidForChunkSize(kChunkSize));

  invalid = valid;
  invalid.index = 2;
  EXPECT_FALSE(invalid.IsValidForChunkSize(std::numeric_limits<uint64_t>::max()));

  uint64_t start_offset = 0;
  EXPECT_TRUE(swordfs::metadata::CalculateChunkStartOffset(1, kChunkSize, &start_offset).ok());
  EXPECT_EQ(start_offset, kChunkSize);
  EXPECT_FALSE(
      swordfs::metadata::CalculateChunkStartOffset(2, std::numeric_limits<uint64_t>::max(), &start_offset).ok());
  EXPECT_EQ(swordfs::metadata::CalculateChunkStartOffset(0, kChunkSize, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(swordfs::metadata::CalculateChunkStartOffset(0, 0, &start_offset).ToErrno(), EINVAL);
}

TEST(SwordFsChunkDescriptorTest, ChunkIndexCoversSupportedOffTRangeAtMinimumChunkSize) {
  constexpr uint64_t kMinimumSupportedChunkSize = 4ULL * 1024;
  const uint64_t max_supported_index =
      static_cast<uint64_t>(std::numeric_limits<off_t>::max()) / kMinimumSupportedChunkSize;

  EXPECT_GE(static_cast<uint64_t>(std::numeric_limits<ChunkIndex>::max()), max_supported_index);
}

TEST(SwordFsChunkDescriptorTest, MapsHighOffsetsWithoutNarrowingAndChecksFileRange) {
  constexpr uint64_t kChunkSize = 64ULL * 1024 * 1024;
  constexpr off_t kGeneric525Offset = std::numeric_limits<off_t>::max() - 1;

  swordfs::metadata::ChunkPosition position;
  ASSERT_TRUE(swordfs::metadata::CalculateChunkPosition(kGeneric525Offset, kChunkSize, &position).ok());
  EXPECT_EQ(position.index, 137438953471ULL);
  EXPECT_EQ(position.start_offset, 9223372036787666944ULL);
  EXPECT_EQ(position.offset_in_chunk, 67108862ULL);

  constexpr uint64_t kLastUint32Index = std::numeric_limits<uint32_t>::max();
  const auto last_uint32_offset = static_cast<off_t>(kLastUint32Index * kChunkSize);
  ASSERT_TRUE(swordfs::metadata::CalculateChunkPosition(last_uint32_offset, kChunkSize, &position).ok());
  EXPECT_EQ(position.index, kLastUint32Index);
  EXPECT_EQ(position.offset_in_chunk, 0U);

  constexpr uint64_t kFirstIndexAboveUint32 = uint64_t{1} << 32;
  const auto old_boundary_offset = static_cast<off_t>(kFirstIndexAboveUint32 * kChunkSize);
  ASSERT_TRUE(swordfs::metadata::CalculateChunkPosition(old_boundary_offset, kChunkSize, &position).ok());
  EXPECT_EQ(position.index, kFirstIndexAboveUint32);
  EXPECT_EQ(position.offset_in_chunk, 0U);

  EXPECT_EQ(swordfs::metadata::CalculateChunkPosition(-1, kChunkSize, &position).ToErrno(), EINVAL);
  EXPECT_EQ(swordfs::metadata::CalculateChunkPosition(0, 0, &position).ToErrno(), EINVAL);
  EXPECT_EQ(swordfs::metadata::CalculateChunkPosition(0, kChunkSize, nullptr).ToErrno(), EINVAL);

  uint64_t range_end = 0;
  ASSERT_TRUE(swordfs::metadata::CalculateFileRangeEnd(kGeneric525Offset, 1, &range_end).ok());
  EXPECT_EQ(range_end, swordfs::metadata::kMaxSupportedFileSize);
  EXPECT_EQ(swordfs::metadata::CalculateFileRangeEnd(kGeneric525Offset, 2, &range_end).ToErrno(), EINVAL);
}

TEST(SwordFsChunkDescriptorTest, ExtentEndingAtOffTMaxIsValidAndBeyondIsRejected) {
  constexpr uint64_t kChunkSize = 4096;
  constexpr ChunkIndex kLastIndex = swordfs::metadata::kMaxSupportedFileSize / kChunkSize;
  constexpr uint64_t kLastStart = kLastIndex * kChunkSize;
  constexpr uint64_t kExactSize = swordfs::metadata::kMaxSupportedFileSize - kLastStart;

  const SwordFsChunk exact{.index = kLastIndex, .revision = 1, .size = kExactSize};
  EXPECT_TRUE(exact.IsValidForChunkSize(kChunkSize));

  const SwordFsChunk beyond{.index = kLastIndex, .revision = 1, .size = kExactSize + 1};
  EXPECT_FALSE(beyond.IsValidForChunkSize(kChunkSize));
}

TEST(ChunkOverwriteStrategyTest, FactoryUsesTypedMechanismSelection) {
  using swordfs::metadata::ChunkOverwriteMechanism;

  std::unique_ptr<swordfs::chunk::IChunkOverwriteStrategy> strategy;
  auto status = swordfs::chunk::CreateChunkOverwriteStrategy(ChunkOverwriteMechanism::kWholeObject, &strategy);
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_NE(strategy, nullptr);
  EXPECT_EQ(strategy->mechanism(), ChunkOverwriteMechanism::kWholeObject);
  EXPECT_EQ(strategy->BindPrivateMetadata(nullptr).ToErrno(), EINVAL);
  NoopPrivateMetadataStore private_metadata;
  EXPECT_TRUE(strategy->BindPrivateMetadata(&private_metadata).ok());

  EXPECT_EQ(swordfs::chunk::CreateChunkOverwriteStrategy(ChunkOverwriteMechanism::kChunkSlice, &strategy).ToErrno(),
            ENOSYS);
  EXPECT_EQ(strategy, nullptr);
  EXPECT_EQ(swordfs::chunk::CreateChunkOverwriteStrategy(static_cast<ChunkOverwriteMechanism>(99), &strategy).ToErrno(),
            ENOSYS);
  EXPECT_EQ(swordfs::chunk::CreateChunkOverwriteStrategy(ChunkOverwriteMechanism::kWholeObject, nullptr).ToErrno(),
            EINVAL);
}

TEST_F(ChunkTest, WriteRejectsOffsetsOutsideItsLogicalChunk) {
  RunInTestFiber([&] {
    Chunk c(/*ino=*/42, /*index=*/1);
    ASSERT_TRUE(c.Initialize().ok());

    EXPECT_EQ(c.Write(-1, Buf("x")).ToErrno(), EINVAL);
    EXPECT_EQ(c.Write(0, Buf("x")).ToErrno(), EINVAL);
  });
}

TEST_F(ChunkTest, WriteRejectsRangeBeyondSupportedFileSize) {
  RunInTestFiber([&] {
    constexpr off_t kLastOffset = std::numeric_limits<off_t>::max();
    constexpr ChunkIndex kLastIndex = static_cast<uint64_t>(kLastOffset) / kChunkTestSize;
    Chunk c(/*ino=*/42, kLastIndex);
    ASSERT_TRUE(c.Initialize().ok());

    EXPECT_EQ(c.Write(kLastOffset, Buf("x")).ToErrno(), EINVAL);
  });
}

// Regression: an uninitialised max_chunk_size_ used to make chunk
// indices > 0 write to byte 0 of their write buffer (StartOffset = 0),
// pushing 64 MiB of file-level data at offset 0 instead of the
// chunk-relative 0 and tripping "write exceeds capacity". The
// fix is that Chunk's constructor must snapshot chunk_size into
// max_chunk_size_ exactly once.
TEST_F(ChunkTest, WriteAtChunkIndexOneStaysWithinCapacity) {
  RunInTestFiber([&] {
    Chunk c(/*ino=*/42, /*index=*/1);
    ASSERT_TRUE(c.Initialize().ok());
    auto buf = *folly::IOBuf::copyBuffer("hello", 5);
    EXPECT_TRUE(c.Write(1024, buf).ok()) << "Chunk::Write at file offset 1024 (chunk-relative 0) "
                                            "must succeed when chunk_size=1024";
  });
}

TEST_F(ChunkTest, WriteBeyondChunkCapacityIsRejected) {
  RunInTestFiber([&] {
    Chunk c(42, 0);
    ASSERT_TRUE(c.Initialize().ok());
    std::string too_big(1025, 'x');
    auto buf = *folly::IOBuf::copyBuffer(too_big.data(), too_big.size());
    auto status = c.Write(0, buf);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.ToErrno(), EINVAL);
  });
}

TEST_F(ChunkTest, EmptyDirtyChunkIsNotFlushableAndFlushIsNoOp) {
  RunInTestFiber([&] {
    Chunk c(42, 0);
    ASSERT_TRUE(c.Initialize().ok());
    EXPECT_FALSE(c.Flushable());
    EXPECT_TRUE(c.Flush().ok());
    EXPECT_FALSE(c.IsClean());
  });
}

TEST_F(ChunkTest, TruncateToCurrentDirtySizeKeepsChunkFlushable) {
  RunInTestFiber([&] {
    Chunk c(42, 0);
    ASSERT_TRUE(c.Initialize().ok());
    auto buf = *folly::IOBuf::copyBuffer("hello", 5);
    ASSERT_TRUE(c.Write(0, buf).ok());
    c.Truncate(5);
    EXPECT_TRUE(c.Flushable());
  });
}

TEST_F(ChunkTest, DirtyReadRejectsNegativeOffsetWithoutAppending) {
  RunInTestFiber([&] {
    Chunk c(42, 0);
    ASSERT_TRUE(c.Initialize().ok());
    ASSERT_TRUE(c.Write(0, Buf("hello")).ok());

    auto out = folly::IOBuf::create(8);
    const auto status = c.Read(-1, 1, out.get());
    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(out->length(), 0U);
  });
}

TEST_F(ChunkTest, DirtyReadFailsClosedOnShortLocalRange) {
  RunInTestFiber([&] {
    Chunk c(42, 0);
    ASSERT_TRUE(c.Initialize().ok());
    ASSERT_TRUE(c.Write(0, Buf("hello")).ok());

    auto out = folly::IOBuf::create(8);
    const auto status = c.Read(0, 8, out.get());
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0U);
  });
}

TEST_F(ChunkTest, SuccessfulFlushBecomesCleanAndSecondFlushIsNoOp) {
  RunInTestFiber([&] {
    Chunk c(42, 0);
    ASSERT_TRUE(c.Initialize().ok());
    ASSERT_TRUE(c.Write(0, Buf("hello")).ok());
    ASSERT_TRUE(c.Flush().ok());
    EXPECT_TRUE(c.IsClean());
    EXPECT_EQ(data_->put_calls, 1);

    EXPECT_TRUE(c.Flush().ok());
    EXPECT_EQ(data_->put_calls, 1);
  });
}

TEST_F(ChunkTest, ConcurrentFlushOnSameChunkReturnsBusy) {
  Chunk c(42, 0);
  RunInTestFiber([&] {
    ASSERT_TRUE(c.Initialize().ok());
    ASSERT_TRUE(c.Write(0, Buf("hello")).ok());
  });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  data_->BlockNextPut(&put_started, &release_put);

  Status first_status;
  Status second_status;
  fm.addTask([&] {
    first_status = c.Flush();
    first_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    second_status = c.Flush();
    second_done.post();
  });

  const bool second_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return second_done.try_wait(); });
  EXPECT_TRUE(second_completed) << "concurrent Flush must report EBUSY within the test watchdog";
  if (!second_completed) {
    release_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return first_done.try_wait() && second_done.try_wait(); }, "Chunk concurrent-flush timeout cleanup");
    return;
  }
  EXPECT_EQ(second_status.ToErrno(), EBUSY);
  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_done.try_wait(); }, "Chunk first Flush completion after release");
  EXPECT_TRUE(first_status.ok());
  RunInTestFiber([&] { EXPECT_TRUE(c.IsClean()); });
}
