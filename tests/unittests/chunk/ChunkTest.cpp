// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for Chunk — focused on the invariant between
// `max_chunk_size_` (the chunk's notion of its own size, used to
// compute StartOffset) and the write buffer's capacity. If those
// drift, writes that cross what the buffer thinks is "beyond capacity"
// but stay within what the chunk thinks is "in range" return EINVAL
// even though the chunk is in kWriting state.

#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "chunk/Chunk.hpp"
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
using swordfs::metadata::RenameResult;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::utils::Status;

namespace {

// Minimal meta engine: FindChunk always returns NotFound so the chunk
// transitions to kWriting and allocates a write buffer.
class MissingMetaEngine final : public IMetaEngine {
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
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view, swordfs::metadata::UnlinkResult *) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag, RenameResult *) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) override {
    return Status::OK();
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Access(InodeID, uint32_t) override {
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
  Status Open(InodeID) override {
    return Status::OK();
  }
  Status StartSession() override {
    return Status::OK();
  }
  Status RefreshSession() override {
    return Status::OK();
  }
  Status StopSession() override {
    return Status::OK();
  }
  Status AcquireOpen(InodeID) override {
    return Status::OK();
  }
  Status ReleaseOpen(InodeID, bool *reclaimable) override {
    if (reclaimable) {
      *reclaimable = false;
    }
    return Status::OK();
  }
  Status ReapStaleSessions() override {
    return Status::OK();
  }
  Status ReclaimInode(InodeID) override {
    return Status::OK();
  }
  Status VisitOrphanedInodes(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitReclaimChunks(InodeID ino, const swordfs::metadata::ChunkVisitorFn &visitor) override {
    return VisitChunks(ino, visitor);
  }
  Status CompleteReclaim(InodeID) override {
    return Status::OK();
  }
  Status VisitChunks(InodeID, const swordfs::metadata::ChunkVisitorFn &) override {
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }
  Status AddChunk(InodeID, const SwordFsChunk &) override {
    return Status::OK();
  }
  Status Truncate(InodeID, uint64_t) override {
    return Status::OK();
  }
};

// Minimal data engine: nothing is actually persisted; the chunk only
// calls Put on Flush, and these tests don't reach that point.
class NullDataEngine final : public IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  DataEngineLimits Limits() const override {
    return {};
  }
  bool Head(std::string_view, size_t *) override {
    return false;
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::NotFound("nope");
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }
};

void InstallEngines() {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_meta_engine(std::make_unique<MissingMetaEngine>());
  vol.set_data_engine(std::make_unique<NullDataEngine>());
}

}  // namespace

class ChunkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::volume::VolumeImpl::Initialize();
    InstallEngines();
  }
};

// Regression: an uninitialised max_chunk_size_ used to make chunk
// indices > 0 write to byte 0 of their write buffer (StartOffset = 0),
// pushing 64 MiB of file-level data at offset 0 instead of the
// chunk-relative 0 and tripping "write exceeds capacity". The
// fix is that Chunk's constructor must snapshot chunk_size into
// max_chunk_size_ exactly once.
TEST_F(ChunkTest, WriteAtChunkIndexOneStaysWithinCapacity) {
  swordfs::volume::VolumeImpl::Instance().set_chunk_size_for_test(1024);

  Chunk c(/*ino=*/42, /*index=*/1);
  ASSERT_TRUE(c.Initialize().ok());

  // Writing up to (but not exceeding) the chunk's capacity must
  // succeed at chunk-relative offset 0, which corresponds to
  // file offset = 1 * 1024 = 1024.
  auto buf = *folly::IOBuf::copyBuffer("hello", 5);
  EXPECT_TRUE(c.Write(1024, buf).ok()) << "Chunk::Write at file offset 1024 (chunk-relative 0) "
                                          "must succeed when chunk_size=1024";
}

TEST_F(ChunkTest, WriteBeyondChunkCapacityIsRejected) {
  swordfs::volume::VolumeImpl::Instance().set_chunk_size_for_test(1024);

  Chunk c(42, 0);
  ASSERT_TRUE(c.Initialize().ok());

  // One byte over the chunk's 1024-byte capacity must fail loudly.
  std::string too_big(1025, 'x');
  auto buf = *folly::IOBuf::copyBuffer(too_big.data(), too_big.size());
  auto status = c.Write(0, buf);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), Status::kInvalidArgument);
}
