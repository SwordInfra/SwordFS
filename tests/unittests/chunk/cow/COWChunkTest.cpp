// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for the stable logical Chunk contract and the current
// COW implementation behind it.

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
#include "chunk/ChunkFactory.hpp"
#include "chunk/cow/COWChunk.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "chunk/cow/WriteBuf.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::chunk::Chunk;
using swordfs::chunk::cow::COWChunk;
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
  Status FindChunk(InodeID, ChunkIndex index, SwordFsChunk *out) override {
    if (!find_chunk_status.ok()) {
      return find_chunk_status;
    }
    if (published_.has_value() && published_->index == index) {
      if (out != nullptr) {
        *out = *published_;
      }
      return Status::OK();
    }
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
  Status Unlink(InodeID, std::string_view, std::optional<InodeID> = std::nullopt) override {
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
      *size = 0;
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
  Status CommitChunk(InodeID, const std::optional<SwordFsChunk> &, const SwordFsChunk &replacement) override {
    published_ = replacement;
    return Status::OK();
  }
  Status Truncate(InodeID, uint64_t) override {
    return Status::OK();
  }

  Status find_chunk_status = Status::OK();

 private:
  swordfs::metadata::ChunkRevision next_revision_ = 1;
  std::optional<SwordFsChunk> published_;
};

// Minimal data engine used to observe flush generations and inject a
// deterministic Put failure without exposing production-only test seams.
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
    if (!next_put_status.ok()) {
      auto status = next_put_status;
      next_put_status = Status::OK();
      return status;
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
  Status next_put_status = Status::OK();

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
    meta_ = dynamic_cast<MissingMetaEngine *>(swordfs::volume::VolumeImpl::Instance().meta_engine());
    ASSERT_NE(meta_, nullptr);
    cow_metadata_ = std::make_shared<swordfs::metadata::MemCOWChunkMetadata>();
  }

  std::unique_ptr<COWChunk> MakeChunk(ChunkIndex index = 0) {
    return std::make_unique<COWChunk>(/*ino=*/42, index, kChunkTestSize, cow_metadata_,
                                      swordfs::volume::VolumeImpl::Instance().meta_engine(), data_, std::nullopt);
  }

  std::shared_ptr<swordfs::metadata::MemCOWChunkMetadata> cow_metadata_;
  MissingMetaEngine *meta_ = nullptr;
  NullDataEngine *data_ = nullptr;
};

TEST(ChunkObjectKeyTest, IncludesInodeIndexAndRevision) {
  EXPECT_EQ(swordfs::chunk::cow::FormatCOWObjectKey(/*ino=*/42, /*index=*/3, /*revision=*/7), "42/3/7");
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

TEST_F(ChunkTest, FactoryOpenDistinguishesMissingExistingAndCreate) {
  RunInTestFiber([&] {
    const auto *factory = swordfs::volume::VolumeImpl::Instance().chunk_factory();
    ASSERT_NE(factory, nullptr);

    std::shared_ptr<Chunk> chunk;
    ASSERT_TRUE(factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/false, &chunk).ok());
    EXPECT_EQ(chunk, nullptr);

    ASSERT_TRUE(factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk).ok());
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->Index(), 0U);
    EXPECT_FALSE(chunk->HasPendingWrites());
    ASSERT_TRUE(chunk->Write(/*offset=*/0, Buf("hello")).ok());
    ASSERT_TRUE(chunk->Flush().ok());

    chunk.reset();
    ASSERT_TRUE(factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/false, &chunk).ok());
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->Index(), 0U);
    EXPECT_FALSE(chunk->HasPendingWrites());

    EXPECT_EQ(factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/false, nullptr).ToErrno(), EINVAL);
  });
}

TEST_F(ChunkTest, FactoryRejectsInvalidRuntimeCompositionBeforeLookup) {
  std::shared_ptr<Chunk> chunk;
  auto cow_metadata = std::make_shared<swordfs::test::StubChunkMetadata>(swordfs::metadata::ChunkType::kCow);

  swordfs::chunk::ChunkFactory missing_meta(swordfs::metadata::ChunkType::kCow, cow_metadata, nullptr, data_,
                                            kChunkTestSize);
  auto status = missing_meta.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "ChunkFactory is not fully initialized");

  swordfs::chunk::ChunkFactory missing_data(swordfs::metadata::ChunkType::kCow, cow_metadata, meta_, nullptr,
                                            kChunkTestSize);
  status = missing_data.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "ChunkFactory is not fully initialized");

  swordfs::chunk::ChunkFactory missing_metadata(swordfs::metadata::ChunkType::kCow, nullptr, meta_, data_,
                                                kChunkTestSize);
  status = missing_metadata.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "ChunkFactory is not fully initialized");

  swordfs::chunk::ChunkFactory missing_chunk_size(swordfs::metadata::ChunkType::kCow, cow_metadata, meta_, data_, 0);
  status = missing_chunk_size.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "ChunkFactory is not fully initialized");

  swordfs::chunk::ChunkFactory wrong_cow_capability(swordfs::metadata::ChunkType::kCow, cow_metadata, meta_, data_,
                                                    kChunkTestSize);
  status = wrong_cow_capability.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "ChunkFactory COW metadata capability mismatch");

  auto slice_metadata = std::make_shared<swordfs::test::StubChunkMetadata>(swordfs::metadata::ChunkType::kChunkSlice);
  swordfs::chunk::ChunkFactory mismatched_metadata(swordfs::metadata::ChunkType::kCow, slice_metadata, meta_, data_,
                                                   kChunkTestSize);
  status = mismatched_metadata.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "ChunkFactory chunk metadata type mismatch");

  swordfs::chunk::ChunkFactory unsupported(swordfs::metadata::ChunkType::kChunkSlice, std::move(slice_metadata), meta_,
                                           data_, kChunkTestSize);
  status = unsupported.Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);
  EXPECT_EQ(status.ToErrno(), ENOSYS);
}

TEST_F(ChunkTest, FactoryOpenPropagatesLookupErrorsWithoutReturningAChunk) {
  RunInTestFiber([&] {
    const auto *factory = swordfs::volume::VolumeImpl::Instance().chunk_factory();
    ASSERT_NE(factory, nullptr);
    meta_->find_chunk_status = Status::IOError("injected lookup failure");

    std::shared_ptr<Chunk> chunk;
    const auto status = factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/true, &chunk);

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(chunk, nullptr);
  });
}

TEST_F(ChunkTest, FactoryOpenRejectsMalformedPersistedDescriptors) {
  RunInTestFiber([&] {
    const auto *factory = swordfs::volume::VolumeImpl::Instance().chunk_factory();
    ASSERT_NE(factory, nullptr);
    const SwordFsChunk malformed{.index = 0, .revision = 1, .size = kChunkTestSize + 1};
    ASSERT_TRUE(meta_->CommitChunk(/*ino=*/42, std::nullopt, malformed).ok());

    std::shared_ptr<Chunk> chunk;
    const auto status = factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/false, &chunk);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(chunk, nullptr);
  });
}

TEST_F(ChunkTest, ReadRejectsInvalidOutputBuffersBeforeAccessingChunkState) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    EXPECT_EQ(chunk->Read(/*offset=*/0, /*len=*/1, nullptr).ToErrno(), EINVAL);

    auto too_small = folly::IOBuf::create(1);
    EXPECT_EQ(chunk->Read(/*offset=*/0, /*len=*/2, too_small.get()).ToErrno(), EINVAL);
    EXPECT_EQ(too_small->length(), 0U);
  });
}

TEST_F(ChunkTest, WriteRejectsMalformedPublishedDescriptorBeforeHydration) {
  RunInTestFiber([&] {
    const SwordFsChunk malformed{.index = 0, .revision = 1, .size = kChunkTestSize + 1};
    COWChunk chunk(/*ino=*/42, /*index=*/0, kChunkTestSize, cow_metadata_, meta_, data_, malformed);
    const auto status = chunk.Write(/*offset=*/0, Buf("x"));
    EXPECT_FALSE(status.ok());
    EXPECT_NE(status.message().find("published chunk exceeds configured chunk size"), std::string::npos);
  });
}

TEST_F(ChunkTest, WriteUsesChunkRelativeOffsets) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk(/*index=*/1);
    EXPECT_TRUE(chunk->Write(/*offset=*/0, Buf("hello")).ok());
    EXPECT_EQ(chunk->Write(kChunkTestSize, Buf("x")).ToErrno(), EINVAL);
    EXPECT_EQ(chunk->Write(kChunkTestSize - 1, Buf("xx")).ToErrno(), EINVAL);
  });
}

TEST_F(ChunkTest, WriteBeyondChunkCapacityIsRejected) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    std::string too_big(kChunkTestSize + 1, 'x');
    auto buf = *folly::IOBuf::copyBuffer(too_big.data(), too_big.size());
    const auto status = chunk->Write(0, buf);
    EXPECT_EQ(status.ToErrno(), EINVAL);
  });
}

TEST_F(ChunkTest, EmptyChunkHasNoPendingWritesAndFlushIsNoOp) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    EXPECT_FALSE(chunk->HasPendingWrites());
    EXPECT_TRUE(chunk->Flush().ok());
    EXPECT_FALSE(chunk->HasPendingWrites());
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(ChunkTest, TruncateToCurrentDirtySizeKeepsPendingWrites) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());
    chunk->TruncateLocal(5);
    EXPECT_TRUE(chunk->HasPendingWrites());
  });
}

TEST_F(ChunkTest, DirtyReadReturnsExactRangeAndZeroFillsTail) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());

    auto out = folly::IOBuf::create(8);
    const auto status = chunk->Read(0, 8, out.get());
    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_EQ(out->length(), 8U);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(out->data()), 5), "hello");
    EXPECT_EQ(out->data()[5], 0U);
    EXPECT_EQ(out->data()[6], 0U);
    EXPECT_EQ(out->data()[7], 0U);
  });
}

TEST_F(ChunkTest, DirtyReadInsideMaterializedChunkZeroFillsUncoveredRange) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());

    auto out = folly::IOBuf::create(4);
    const auto status = chunk->Read(7, 4, out.get());
    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_EQ(out->length(), 4U);
    for (size_t i = 0; i < out->length(); ++i) {
      EXPECT_EQ(out->data()[i], 0U);
    }
  });
}

TEST_F(ChunkTest, ReadRejectsRangeCrossingChunkBoundaryWithoutAppending) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    auto out = folly::IOBuf::create(2);
    const auto status = chunk->Read(kChunkTestSize - 1, 2, out.get());
    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(out->length(), 0U);
  });
}

TEST_F(ChunkTest, SuccessfulFlushClearsPendingWritesAndSecondFlushIsNoOp) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());
    EXPECT_TRUE(chunk->HasPendingWrites());
    ASSERT_TRUE(chunk->Flush().ok());
    EXPECT_FALSE(chunk->HasPendingWrites());
    EXPECT_EQ(data_->put_calls, 1);

    EXPECT_TRUE(chunk->Flush().ok());
    EXPECT_EQ(data_->put_calls, 1);
  });
}

TEST_F(ChunkTest, LegacyFlushRemainsSoleAuthorityWhileTypedCowCapabilityIsOnlyInjected) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());
    ASSERT_TRUE(chunk->Flush().ok());

    SwordFsChunk published;
    ASSERT_TRUE(meta_->FindChunk(/*ino=*/42, /*index=*/0, &published).ok());
    EXPECT_EQ(published.revision, 1U);
    EXPECT_EQ(published.size, 5U);

    swordfs::metadata::ChunkID first_chunk_id;
    ASSERT_TRUE(cow_metadata_->AllocateChunkID(&first_chunk_id).ok());
    EXPECT_EQ(first_chunk_id, swordfs::metadata::ChunkID(1));
    swordfs::metadata::cow::COWChunkHead staged_head;
    EXPECT_TRUE(cow_metadata_->GetHead(first_chunk_id, &staged_head).IsNotFound());
  });
}

TEST_F(ChunkTest, FailedFlushKeepsPendingWritesUntilLaterSuccess) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());
    data_->next_put_status = Status::IOError("injected put failure");

    EXPECT_EQ(chunk->Flush().ToErrno(), EIO);
    EXPECT_TRUE(chunk->HasPendingWrites());

    ASSERT_TRUE(chunk->Flush().ok());
    EXPECT_FALSE(chunk->HasPendingWrites());
    EXPECT_EQ(data_->put_calls, 2);
  });
}

TEST_F(ChunkTest, WriteDuringFlushRemainsPendingAfterEarlierGenerationAck) {
  auto chunk = MakeChunk();
  RunInTestFiber([&] { ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok()); });

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
    flush_status = chunk->Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    write_status = chunk->Write(5, Buf("!"));
    write_done.post();
    release_put.post();
  });

  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && write_done.try_wait(); }, "Chunk write-during-flush completion");

  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_TRUE(write_status.ok()) << write_status.message();
  RunInTestFiber([&] {
    EXPECT_TRUE(chunk->HasPendingWrites())
        << "the first flush ack must not clear writes accepted into the next generation";
  });

  RunInTestFiber([&] {
    ASSERT_TRUE(chunk->Flush().ok());
    EXPECT_FALSE(chunk->HasPendingWrites());
  });
  EXPECT_EQ(data_->put_calls, 2);
}

TEST_F(ChunkTest, ConcurrentFlushOnSameChunkReturnsBusyAndKeepsPendingStateUntilAck) {
  auto chunk = MakeChunk();
  RunInTestFiber([&] { ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  data_->BlockNextPut(&put_started, &release_put);

  Status first_status;
  Status second_status;
  bool pending_while_flushing = false;
  fm.addTask([&] {
    first_status = chunk->Flush();
    first_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    pending_while_flushing = chunk->HasPendingWrites();
    second_status = chunk->Flush();
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
  EXPECT_TRUE(pending_while_flushing);
  EXPECT_EQ(second_status.ToErrno(), EBUSY);
  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_done.try_wait(); }, "Chunk first Flush completion after release");
  EXPECT_TRUE(first_status.ok());
  RunInTestFiber([&] { EXPECT_FALSE(chunk->HasPendingWrites()); });
}
