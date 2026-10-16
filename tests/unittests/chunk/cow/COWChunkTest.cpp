// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for the stable logical Chunk contract and the current
// COW implementation behind it.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "FiberTest.hpp"
#include "TestCOWChunkMetadata.hpp"
#include "TestWatchdog.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/Chunk.hpp"
#include "chunk/ChunkFactory.hpp"
#include "chunk/cow/COWChunk.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "chunk/cow/WriteBuf.hpp"
#include "chunk/slice/SliceRecord.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"
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
  // Everything else is irrelevant for these tests.
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID, SwordFsInode *) override {
    return Status::OK();
  }
  Status ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, swordfs::metadata::FileChunkSnapshot *out) override {
    if (!file_snapshot_status.ok()) {
      return file_snapshot_status;
    }
    if (out == nullptr) {
      return Status::InvalidArgument("typed chunk snapshot output is null");
    }
    swordfs::metadata::FileChunkSnapshot snapshot;
    snapshot.inode = SwordFsInode(ino, SwordFsAttr(ino, S_IFREG | 0644), ino);
    snapshot.inode.attr.size = file_size_;
    if (auto it = typed_mappings_.find(index); it != typed_mappings_.end()) {
      snapshot.chunk_id = it->second;
    }
    swordfs::metadata::ChunkSizeLayout layout;
    auto status = swordfs::metadata::PlanChunkSizeLayout(file_size_, kChunkTestSize, &layout);
    if (!status.ok()) {
      return status;
    }
    if (layout.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> boundary_id;
      if (auto it = typed_mappings_.find(layout.boundary->index); it != typed_mappings_.end()) {
        boundary_id = it->second;
      }
      snapshot.eof_boundary = swordfs::metadata::ChunkBoundarySnapshot{
          .index = layout.boundary->index, .chunk_id = boundary_id, .visible_prefix = layout.boundary->visible_prefix};
    }
    *out = std::move(snapshot);
    return Status::OK();
  }
  Status ReadFileMappingSnapshot(InodeID, swordfs::metadata::FileMappingSnapshot *) override {
    return Status::NotSupported("whole-file size mutations are outside these chunk-focused tests");
  }
  Status CommitShrink(InodeID, const swordfs::metadata::ChunkSizePlan &, const SwordFsAttr &, SetAttrField,
                      swordfs::metadata::ChunkSizeCommitResult *) override {
    return Status::NotSupported("whole-file size mutations are outside these chunk-focused tests");
  }
  Status CommitGrow(InodeID, const swordfs::metadata::ChunkSizePlan &, const SwordFsAttr &, SetAttrField,
                    swordfs::metadata::ChunkSizeCommitResult *) override {
    return Status::NotSupported("whole-file size mutations are outside these chunk-focused tests");
  }
  Status ProbeAttachment(InodeID, ChunkIndex index, std::optional<swordfs::metadata::ChunkID> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("typed attachment output is null");
    }
    out->reset();
    if (auto it = typed_mappings_.find(index); it != typed_mappings_.end()) {
      *out = it->second;
    }
    return Status::OK();
  }
  Status AttachPrepared(InodeID, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                        const swordfs::metadata::FileSizePrecondition &expected) override {
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, kChunkTestSize);
    if (!status.ok()) {
      return status;
    }
    status = ValidateExpectedFileState(expected);
    if (!status.ok()) {
      return status;
    }
    if (typed_mappings_.contains(index)) {
      return Status::AlreadyExists("attachment already won");
    }
    typed_mappings_.emplace(index, id);
    file_size_ = std::max(file_size_, end);
    return Status::OK();
  }
  // Simulate an independently committed FileMetadata replacement in tests.
  void ReplaceAttachment(ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t eof) {
    typed_mappings_[index] = id;
    file_size_ = eof;
  }
  Status FinalizeAttachedWrite(InodeID, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                               const swordfs::metadata::FileSizePrecondition &expected) override {
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, kChunkTestSize);
    if (!status.ok()) {
      return status;
    }
    status = ValidateExpectedFileState(expected);
    if (!status.ok()) {
      return status;
    }
    auto it = typed_mappings_.find(index);
    if (it == typed_mappings_.end() || it->second != id) {
      return Status::AlreadyExists("attached identity changed");
    }
    file_size_ = std::max(file_size_, end);
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
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }

 private:
  Status ValidateExpectedFileState(const swordfs::metadata::FileSizePrecondition &expected) const {
    auto status = swordfs::metadata::ValidateFileSizePrecondition(expected, kChunkTestSize);
    if (!status.ok()) {
      return status;
    }
    if (expected.eof != file_size_) {
      return Status::AlreadyExists("EOF changed");
    }
    if (expected.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> current;
      if (auto it = typed_mappings_.find(expected.boundary->index); it != typed_mappings_.end()) {
        current = it->second;
      }
      if (current != expected.boundary->chunk_id) {
        return Status::AlreadyExists("EOF boundary changed");
      }
    }
    return Status::OK();
  }

  uint64_t file_size_ = 0;
  std::map<ChunkIndex, swordfs::metadata::ChunkID> typed_mappings_;

 public:
  Status file_snapshot_status = Status::OK();
};

// Minimal data engine used to observe flush generations and inject a
// deterministic Put failure without exposing production-only test seams.
class NullDataEngine final : public IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    ++put_calls;
    uploaded_keys.emplace_back(key);
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
    blobs_.insert_or_assign(std::string(key),
                            std::string(reinterpret_cast<const char *>(data->data()), data->length()));
    return Status::OK();
  }
  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) override {
    ++get_calls;
    requested_bytes += size;
    if (fail_get_call == get_calls) {
      return Status::IOError("injected later physical GET failure");
    }
    const auto it = blobs_.find(std::string(key));
    if (it == blobs_.end()) {
      return Status::NotFound("missing immutable COW blob");
    }
    const auto &blob = it->second;
    if (offset < blob.size()) {
      const size_t available = std::min(size, blob.size() - offset);
      if (available > out->tailroom()) {
        return Status::InvalidArgument("insufficient object-read tailroom");
      }
      std::memcpy(out->writableTail(), blob.data() + offset, available);
      out->append(available);
    }
    return Status::OK();
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }

  void BlockNextPut(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    put_started_ = started;
    put_release_ = release;
  }

  int put_calls = 0;
  int get_calls = 0;
  int fail_get_call = -1;
  size_t requested_bytes = 0;
  std::vector<std::string> uploaded_keys;
  Status next_put_status = Status::OK();

 private:
  std::unordered_map<std::string, std::string> blobs_;
  folly::fibers::Baton *put_started_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
};

// Inject COW private-domain failures without adding test-only APIs to the
// production metadata implementation.
class FaultingCOWChunkMetadata final : public swordfs::metadata::cow::COWChunkMetadata {
 public:
  enum class CASFailure { kNone, kAmbiguousApplied, kAmbiguousNotApplied, kRejected };

  Status AllocateChunkID(swordfs::metadata::ChunkID *out) override {
    if (!allocation_status.ok()) {
      return allocation_status;
    }
    return delegate_.AllocateChunkID(out);
  }
  Status AllocateRevision(swordfs::metadata::ChunkID id, swordfs::metadata::cow::COWChunkRevision *out) override {
    return delegate_.AllocateRevision(id, out);
  }
  Status GetHead(swordfs::metadata::ChunkID id, swordfs::metadata::cow::COWChunkHead *out) override {
    if (!head_lookup_status.ok()) {
      if (successful_lookups_before_error == 0) {
        return head_lookup_status;
      }
      --successful_lookups_before_error;
    }
    return delegate_.GetHead(id, out);
  }
  Status CompareExchangeHead(swordfs::metadata::ChunkID id,
                             const std::optional<swordfs::metadata::cow::COWChunkHead> &expected,
                             const swordfs::metadata::cow::COWChunkHead &replacement) override {
    if (cas_failure == CASFailure::kRejected) {
      return Status::IOError("private head CAS rejected");
    }
    if (cas_failure == CASFailure::kAmbiguousNotApplied) {
      return Status::OutcomeUnknown("private head CAS outcome lost");
    }
    auto status = delegate_.CompareExchangeHead(id, expected, replacement);
    if (status.ok() && cas_failure == CASFailure::kAmbiguousApplied) {
      return Status::OutcomeUnknown("private head CAS reply lost after commit");
    }
    return status;
  }
  Status EraseHead(swordfs::metadata::ChunkID id, const swordfs::metadata::cow::COWChunkHead &expected) override {
    return delegate_.EraseHead(id, expected);
  }

  Status allocation_status = Status::OK();
  Status head_lookup_status = Status::OK();
  int successful_lookups_before_error = 0;
  CASFailure cas_failure = CASFailure::kNone;

 private:
  swordfs::test::TestCOWChunkMetadata delegate_;
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
    cow_metadata_ = std::make_shared<swordfs::test::TestCOWChunkMetadata>();
  }

  std::unique_ptr<COWChunk> MakeChunk(ChunkIndex index = 0) {
    return std::make_unique<COWChunk>(/*ino=*/42, index, kChunkTestSize, cow_metadata_,
                                      swordfs::volume::VolumeImpl::Instance().meta_engine(), data_, std::nullopt);
  }

  std::shared_ptr<swordfs::test::TestCOWChunkMetadata> cow_metadata_;
  MissingMetaEngine *meta_ = nullptr;
  NullDataEngine *data_ = nullptr;
};

TEST(ChunkLayoutTest, ChunkIndexCoversSupportedOffTRangeAtMinimumChunkSize) {
  constexpr uint64_t kMinimumSupportedChunkSize = 4ULL * 1024;
  const uint64_t max_supported_index =
      static_cast<uint64_t>(std::numeric_limits<off_t>::max()) / kMinimumSupportedChunkSize;

  EXPECT_GE(static_cast<uint64_t>(std::numeric_limits<ChunkIndex>::max()), max_supported_index);
}

TEST(ChunkLayoutTest, MapsHighOffsetsWithoutNarrowingAndChecksFileRange) {
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
  EXPECT_EQ(
      missing_meta.SanitizeBoundary(42, {.index = 0, .chunk_id = swordfs::metadata::ChunkID(47), .visible_prefix = 1})
          .ToErrno(),
      ENOSYS);

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

TEST_F(ChunkTest, FactoryBoundarySanitationChecksMissingHeadAgainstLiveAttachment) {
  RunInTestFiber([&] {
    const auto *factory = swordfs::volume::VolumeImpl::Instance().chunk_factory();
    ASSERT_NE(factory, nullptr);
    const swordfs::metadata::RetainedChunkBoundary orphan_head{
        .index = 0, .chunk_id = swordfs::metadata::ChunkID(811), .visible_prefix = 7};
    EXPECT_TRUE(factory->SanitizeBoundary(42, orphan_head).ok())
        << "a missing head that is not FileMetadata-attached must not turn into a file error";
  });
}

TEST_F(ChunkTest, FactoryOpenPropagatesLookupErrorsWithoutReturningAChunk) {
  RunInTestFiber([&] {
    const auto *factory = swordfs::volume::VolumeImpl::Instance().chunk_factory();
    ASSERT_NE(factory, nullptr);
    meta_->file_snapshot_status = Status::IOError("injected lookup failure");

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
    ASSERT_TRUE(
        meta_->AttachPrepared(/*ino=*/42, /*index=*/0, swordfs::metadata::ChunkID(17), /*end=*/5, {.eof = 0}).ok());

    std::shared_ptr<Chunk> chunk;
    const auto status = factory->Open(/*ino=*/42, /*index=*/0, /*create_if_missing=*/false, &chunk);

    EXPECT_EQ(status.ToErrno(), EIO);
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
    const swordfs::metadata::cow::COWChunkHead malformed{.revision = swordfs::metadata::cow::COWChunkRevision(1),
                                                         .size = kChunkTestSize + 1};
    COWChunk chunk(/*ino=*/42, /*index=*/0, kChunkTestSize, cow_metadata_, meta_, data_, swordfs::metadata::ChunkID(55),
                   malformed, kChunkTestSize);
    const auto status = chunk.Write(/*offset=*/0, Buf("x"));
    EXPECT_FALSE(status.ok());
    EXPECT_NE(status.message().find("published chunk exceeds configured chunk size"), std::string::npos);
  });
}

TEST_F(ChunkTest, CleanOverwritePropagatesPrivateHeadLookupFailure) {
  RunInTestFiber([&] {
    auto faulting = std::make_shared<FaultingCOWChunkMetadata>();
    const swordfs::metadata::ChunkID id(57);
    const swordfs::metadata::cow::COWChunkHead head{swordfs::metadata::cow::COWChunkRevision(1), 1};
    ASSERT_TRUE(faulting->CompareExchangeHead(id, std::nullopt, head).ok());
    COWChunk chunk(42, 0, kChunkTestSize, faulting, meta_, data_, id, head, 1);
    faulting->head_lookup_status = Status::IOError("private head temporarily unavailable");
    const auto status = chunk.Write(0, Buf("x"));
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "private head temporarily unavailable");
    EXPECT_FALSE(chunk.HasPendingWrites());
  });
}

TEST_F(ChunkTest, ReadRefreshFailureAfterMissingOldObjectPropagatesMetadataError) {
  RunInTestFiber([&] {
    auto faulting = std::make_shared<FaultingCOWChunkMetadata>();
    const swordfs::metadata::ChunkID id(58);
    const swordfs::metadata::cow::COWChunkHead head{swordfs::metadata::cow::COWChunkRevision(1), 1};
    ASSERT_TRUE(faulting->CompareExchangeHead(id, std::nullopt, head).ok());
    COWChunk chunk(42, 0, kChunkTestSize, faulting, meta_, data_, id, head, 1);
    faulting->successful_lookups_before_error = 1;
    faulting->head_lookup_status = Status::IOError("cannot refresh private head after missing blob");
    auto out = folly::IOBuf::create(1);
    const auto status = chunk.Read(0, 1, out.get());
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "cannot refresh private head after missing blob");
    EXPECT_EQ(out->length(), 0U);
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

TEST_F(ChunkTest, CachedCleanHeadRevisionGrowthDoesNotHideNewBytes) {
  RunInTestFiber([&] {
    const swordfs::metadata::ChunkID id(88);
    const swordfs::metadata::cow::COWChunkHead first{swordfs::metadata::cow::COWChunkRevision(1), 5};
    ASSERT_TRUE(cow_metadata_->CompareExchangeHead(id, std::nullopt, first).ok());
    const swordfs::chunk::cow::COWObjectKey first_key(id, first.revision);
    ASSERT_TRUE(data_->Put(static_cast<std::string_view>(first_key), folly::IOBuf::copyBuffer("hello")).ok());
    COWChunk cached(42, 0, kChunkTestSize, cow_metadata_, meta_, data_, id, first, 5);

    // Another publication grows the same stable ChunkID. The cached clean
    // session must resolve ALL bytes from the new authoritative revision.
    const swordfs::metadata::cow::COWChunkHead latest{swordfs::metadata::cow::COWChunkRevision(2), 10};
    const swordfs::chunk::cow::COWObjectKey latest_key(id, latest.revision);
    ASSERT_TRUE(data_->Put(static_cast<std::string_view>(latest_key), folly::IOBuf::copyBuffer("helloWORLD")).ok());
    ASSERT_TRUE(cow_metadata_->CompareExchangeHead(id, first, latest).ok());

    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(cached.Read(5, 5, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "WORLD")
        << "the old session's visible prefix is not a user-visible revision snapshot";

    ASSERT_TRUE(cached.Write(9, Buf("!")).ok());
    auto after_write = folly::IOBuf::create(5);
    ASSERT_TRUE(cached.Read(5, 5, after_write.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(after_write->data()), after_write->length()), "WORL!")
        << "clean overwrite hydration must not discard newly published tail bytes";
  });
}

TEST_F(ChunkTest, ReadRejectsOversizedPrivateHeadBeforeObjectAccess) {
  RunInTestFiber([&] {
    const swordfs::metadata::cow::COWChunkHead invalid{.revision = swordfs::metadata::cow::COWChunkRevision(1),
                                                       .size = kChunkTestSize + 1};
    const swordfs::metadata::ChunkID id(87);
    ASSERT_TRUE(cow_metadata_->CompareExchangeHead(id, std::nullopt, invalid).ok());
    COWChunk chunk(42, 0, kChunkTestSize, cow_metadata_, meta_, data_, id, invalid, kChunkTestSize);
    auto out = folly::IOBuf::create(1);
    EXPECT_EQ(chunk.Read(0, 1, out.get()).ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0U);
  });
}

TEST_F(ChunkTest, WriteRejectsMissingPrivateHeadForStillAttachedChunk) {
  RunInTestFiber([&] {
    const swordfs::metadata::ChunkID id(87);
    ASSERT_TRUE(meta_->AttachPrepared(42, 0, id, 1, {.eof = 0}).ok());
    const swordfs::metadata::cow::COWChunkHead stale{.revision = swordfs::metadata::cow::COWChunkRevision(1),
                                                     .size = 0};
    COWChunk chunk(42, 0, kChunkTestSize, cow_metadata_, meta_, data_, id, stale, 0);
    EXPECT_EQ(chunk.Write(0, Buf("x")).ToErrno(), EIO);
    EXPECT_FALSE(chunk.HasPendingWrites());
  });
}

TEST_F(ChunkTest, FlushPreservesDirtyGenerationWhenChunkIDAllocationFails) {
  RunInTestFiber([&] {
    auto faulting = std::make_shared<FaultingCOWChunkMetadata>();
    faulting->allocation_status = Status::IOError("allocator unavailable");
    COWChunk chunk(42, 0, kChunkTestSize, faulting, meta_, data_, std::nullopt);
    ASSERT_TRUE(chunk.Write(0, Buf("payload")).ok());
    EXPECT_EQ(chunk.Flush().ToErrno(), EIO);
    EXPECT_TRUE(chunk.HasPendingWrites());
    EXPECT_EQ(data_->put_calls, 0);
  });
}

TEST_F(ChunkTest, FlushReconcilesAppliedPrivateHeadCASWithLostReply) {
  RunInTestFiber([&] {
    auto faulting = std::make_shared<FaultingCOWChunkMetadata>();
    faulting->cas_failure = FaultingCOWChunkMetadata::CASFailure::kAmbiguousApplied;
    COWChunk chunk(42, 0, kChunkTestSize, faulting, meta_, data_, std::nullopt);
    ASSERT_TRUE(chunk.Write(0, Buf("payload")).ok());
    EXPECT_TRUE(chunk.Flush().ok());
    EXPECT_FALSE(chunk.HasPendingWrites());
    EXPECT_EQ(data_->put_calls, 1);
  });
}

TEST_F(ChunkTest, FlushPreservesDirtyGenerationWhenPrivateHeadCASNotApplied) {
  RunInTestFiber([&] {
    auto faulting = std::make_shared<FaultingCOWChunkMetadata>();
    faulting->cas_failure = FaultingCOWChunkMetadata::CASFailure::kAmbiguousNotApplied;
    COWChunk chunk(42, 0, kChunkTestSize, faulting, meta_, data_, std::nullopt);
    ASSERT_TRUE(chunk.Write(0, Buf("payload")).ok());
    EXPECT_TRUE(chunk.Flush().IsOutcomeUnknown());
    EXPECT_TRUE(chunk.HasPendingWrites());
    swordfs::metadata::FileChunkSnapshot file;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(42, 0, &file).ok());
    EXPECT_FALSE(file.chunk_id.has_value());
  });
}

TEST_F(ChunkTest, FlushPreservesDirtyGenerationWhenPrivateHeadCASRejects) {
  RunInTestFiber([&] {
    auto faulting = std::make_shared<FaultingCOWChunkMetadata>();
    faulting->cas_failure = FaultingCOWChunkMetadata::CASFailure::kRejected;
    COWChunk chunk(42, 0, kChunkTestSize, faulting, meta_, data_, std::nullopt);
    ASSERT_TRUE(chunk.Write(0, Buf("payload")).ok());
    EXPECT_EQ(chunk.Flush().ToErrno(), EIO);
    EXPECT_TRUE(chunk.HasPendingWrites());
  });
}

TEST_F(ChunkTest, AttachedRewritePreservesChunkIDAndAdvancesOnlyPrivateHead) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());
    ASSERT_TRUE(chunk->Flush().ok());

    swordfs::metadata::FileChunkSnapshot before;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(/*ino=*/42, 0, &before).ok());
    ASSERT_TRUE(before.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead first;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &first).ok());
    ASSERT_TRUE(chunk->Write(5, Buf("!")).ok());
    ASSERT_TRUE(chunk->Flush().ok());

    swordfs::metadata::FileChunkSnapshot after;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(/*ino=*/42, 0, &after).ok());
    EXPECT_EQ(after.chunk_id, before.chunk_id);
    EXPECT_EQ(after.inode.attr.size, 6U);
    swordfs::metadata::cow::COWChunkHead second;
    ASSERT_TRUE(cow_metadata_->GetHead(*after.chunk_id, &second).ok());
    EXPECT_GT(second.revision.Value(), first.revision.Value());
    EXPECT_EQ(second.size, 6U);
    EXPECT_EQ(data_->uploaded_keys.size(), 2U);
  });
}

TEST_F(ChunkTest, ConcurrentFirstAttachmentsAtDistinctIndexesPreserveBothWrites) {
  auto first = MakeChunk(/*index=*/0);
  auto second = MakeChunk(/*index=*/1);
  RunInTestFiber([&] {
    ASSERT_TRUE(first->Write(0, Buf("first")).ok());
    ASSERT_TRUE(second->Write(0, Buf("second")).ok());
  });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_put_started;
  folly::fibers::Baton release_first_put;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  data_->BlockNextPut(&first_put_started, &release_first_put);
  Status first_status;
  Status second_status;
  fm.addTask([&] {
    first_status = first->Flush();
    first_done.post();
  });
  fm.addTask([&] {
    first_put_started.wait();
    second_status = second->Flush();
    second_done.post();
    release_first_put.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_done.try_wait() && second_done.try_wait(); },
      "parallel distinct-chunk first attachments");
  EXPECT_TRUE(first_status.ok()) << first_status.message();
  EXPECT_TRUE(second_status.ok()) << second_status.message();
  RunInTestFiber([&] {
    swordfs::metadata::FileChunkSnapshot a;
    swordfs::metadata::FileChunkSnapshot b;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(42, 0, &a).ok());
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(42, 1, &b).ok());
    EXPECT_TRUE(a.chunk_id.has_value());
    EXPECT_TRUE(b.chunk_id.has_value());
    EXPECT_EQ(a.inode.attr.size, kChunkTestSize + 6);
    EXPECT_EQ(b.inode.attr.size, kChunkTestSize + 6);
  });
}

TEST_F(ChunkTest, FirstFlushPublishesTypedChunkIdAndPrivateHeadWithoutLegacyDescriptor) {
  RunInTestFiber([&] {
    auto chunk = MakeChunk();
    ASSERT_TRUE(chunk->Write(0, Buf("hello")).ok());
    ASSERT_TRUE(chunk->Flush().ok());

    swordfs::metadata::FileChunkSnapshot file;
    ASSERT_TRUE(meta_->ReadFileChunkSnapshot(/*ino=*/42, /*index=*/0, &file).ok());
    ASSERT_TRUE(file.chunk_id.has_value()) << "FileMetadata must own the attached ChunkID";
    EXPECT_EQ(file.inode.attr.size, 5U);
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*file.chunk_id, &head).ok());
    EXPECT_EQ(head.size, 5U);
    ASSERT_EQ(data_->uploaded_keys.size(), 1U);
    EXPECT_EQ(
        data_->uploaded_keys[0],
        std::string(static_cast<std::string_view>(swordfs::chunk::cow::COWObjectKey(*file.chunk_id, head.revision))));
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

class SliceChunkTest : public ChunkTest {
 protected:
  void SetUp() override {
    ChunkTest::SetUp();
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
