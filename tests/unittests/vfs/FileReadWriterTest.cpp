// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileReadWriter — read, write, flush, and
// cross-file-handle sharing behaviour.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::ChunkMeta;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::RenameResult;
using swordfs::metadata::SetAttrField;
using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::utils::Status;
using swordfs::vfs::FileReadWriter;

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

static std::string Repeat(char c, size_t n) {
  return std::string(n, c);
}

static auto Buf(const std::string &s) {
  return *folly::IOBuf::copyBuffer(s.data(), s.size());
}

template <typename Fn>
static void RunInTestFiber(Fn &&fn) {
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  fm.addTask([&] {
    fn();
    done.post();
  });
  while (!done.try_wait()) {
    evb.loopOnce();
  }
}

// ────────────────────────────────────────────────────────────────
// MockDataEngine — in-memory IDataEngine
// ────────────────────────────────────────────────────────────────

class MockDataEngine : public IDataEngine {
 public:
  DataEngineLimits Limits() const override {
    DataEngineLimits lim;
    lim.supports_multipart = false;
    return lim;
  }

  bool Head(std::string_view key, size_t *size) override {
    auto it = store_.find(std::string(key));
    if (it == store_.end()) {
      return false;
    }
    if (size) {
      *size = it->second.size();
    }
    return true;
  }

  Status Put(std::string_view key,
             std::unique_ptr<folly::IOBuf> data) override {
    store_[std::string(key)] = std::string(
        reinterpret_cast<const char *>(data->data()), data->length());
    return Status::OK();
  }

  Status Get(std::string_view key, size_t offset, size_t size,
             folly::IOBuf *out) override {
    auto it = store_.find(std::string(key));
    if (it == store_.end()) {
      return Status::NotFound("chunk not found");
    }
    const std::string &chunk = it->second;
    if (offset >= chunk.size()) {
      return Status::OK();
    }
    size_t len = (size == 0) ? chunk.size() - offset
                             : std::min(size, chunk.size() - offset);
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

  std::vector<std::string> delete_calls;
  Status delete_status = Status::OK();

 private:
  std::unordered_map<std::string, std::string> store_;
};

// ────────────────────────────────────────────────────────────────
// MockMetaEngine — minimal IMetaEngine with chunk support
// ────────────────────────────────────────────────────────────────

class MockMetaEngine : public IMetaEngine {
 public:
  Status Lookup(InodeID, std::string_view, InodeID *,
                struct stat *) override { return Status::OK(); }
  Status GetAttr(InodeID, struct stat *attr) override {
    std::memset(attr, 0, sizeof(*attr));
    attr->st_size = file_size_;
    return Status::OK();
  }
  Status ReadDir(InodeID,
                 std::vector<swordfs::metadata::SwordFsEntry> *) override {
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, mode_t, InodeID *,
                struct stat *) override { return Status::OK(); }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID *,
               struct stat *) override { return Status::OK(); }
  Status Unlink(InodeID, std::string_view, nlink_t *) override { return Status::OK(); }
  Status RmDir(InodeID, std::string_view) override { return Status::OK(); }
  Status Rename(InodeID, std::string_view, InodeID,
                std::string_view, RenameFlag, RenameResult *) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const struct stat *attr, SetAttrField fields,
                 struct stat *out_attr) override {
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      file_size_ = attr->st_size;
    }
    if (out_attr) {
      std::memset(out_attr, 0, sizeof(*out_attr));
      out_attr->st_size = file_size_;
    }
    return Status::OK();
  }
  Status StatFs(struct statvfs *) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Symlink(InodeID, std::string_view, const char *,
                 InodeID *, struct stat *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view,
              struct stat *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID) override { return Status::OK(); }
  Status ReclaimInode(InodeID) override { return Status::OK(); }
  Status ListChunks(InodeID, std::vector<ChunkMeta> *) override {
    return Status::OK();
  }
  Status OpenDir(InodeID) override { return Status::OK(); }

  Status AddChunk(InodeID ino, const ChunkMeta &cm) override {
    chunks_[ino][cm.index] = cm;
    return Status::OK();
  }

  Status FindChunk(InodeID ino, ChunkIndex idx,
                   ChunkMeta *cm) override {
    auto it = chunks_.find(ino);
    if (it == chunks_.end()) {
      return Status::NotFound("");
    }
    auto cit = it->second.find(idx);
    if (cit == it->second.end()) {
      return Status::NotFound("");
    }
    if (cm) {
      *cm = cit->second;
    }
    return Status::OK();
  }

  Status Truncate(InodeID ino, size_t size) override {
    ++truncate_calls;
    if (!truncate_status_.ok()) {
      return truncate_status_;
    }
    chunks_.erase(ino);
    file_size_ = static_cast<off_t>(size);
    return Status::OK();
  }

  void set_file_size(off_t size) { file_size_ = size; }
  void set_truncate_status(Status s) { truncate_status_ = s; }
  off_t file_size() const { return file_size_; }

  int truncate_calls = 0;

 private:
  off_t file_size_ = 0;
  Status truncate_status_ = Status::OK();
  std::unordered_map<InodeID,
                     std::unordered_map<ChunkIndex, ChunkMeta>>
      chunks_;
};

// ────────────────────────────────────────────────────────────────
// FileReadWriterTest
// ────────────────────────────────────────────────────────────────

class FileReadWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto data = std::make_unique<MockDataEngine>();
    auto meta = std::make_unique<MockMetaEngine>();
    mock_data_ = data.get();
    mock_meta_ = meta.get();
    swordfs::volume::VolumeImpl::Initialize();
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_meta_engine(std::move(meta));
    vol.set_data_engine(std::move(data));
  }

  FileReadWriter Make(off_t file_size = 0) {
    mock_meta_->set_file_size(file_size);
    return FileReadWriter(kIno);
  }

  static constexpr size_t kChunkSize = 1024;
  static constexpr InodeID kIno = 42;
  MockDataEngine *mock_data_ = nullptr;
  MockMetaEngine *mock_meta_ = nullptr;
};

// ────────────────────────────────────────────────────────────────
// Write → Read round-trip (dirty buffer)
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FullChunk) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('A', kChunkSize));
  });
}

TEST_F(FileReadWriterTest, WithinChunkWithOffset) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(100, 200, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('A', 100));
  });
}

TEST_F(FileReadWriterTest, PartialChunkAtEOF) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', 500)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
    std::string expected = Repeat('A', 500) +
                           std::string(kChunkSize - 500, '\0');
    EXPECT_EQ(out->length(), kChunkSize);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              expected);
  });
}

TEST_F(FileReadWriterTest, PastEOF) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(100, kChunkSize + 100, out.get()).ok());
    EXPECT_EQ(out->length(), 100);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              std::string(100, '\0'));
  });
}

// ────────────────────────────────────────────────────────────────
// Cross-chunk reads
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, CrossChunkBoundary) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 500)),
                         static_cast<off_t>(kChunkSize))
                    .ok());

    off_t off = static_cast<off_t>(kChunkSize) - 100;
    std::string expected = Repeat('A', 100) + Repeat('B', 500);
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(600, off, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              expected);
  });
}

TEST_F(FileReadWriterTest, CrossChunkExactBoundary) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 500)),
                         static_cast<off_t>(kChunkSize))
                    .ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(500, static_cast<off_t>(kChunkSize), out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('B', 500));
  });
}

TEST_F(FileReadWriterTest, CrossChunkReadsIntoSecondChunk) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 800)),
                         static_cast<off_t>(kChunkSize))
                    .ok());

    off_t off = static_cast<off_t>(kChunkSize) - 1;
    std::string expected = "A" + Repeat('B', 500);
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(501, off, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              expected);
  });
}

TEST_F(FileReadWriterTest, CrossChunkExhaustsSecondChunk) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 200)),
                         static_cast<off_t>(kChunkSize))
                    .ok());

    off_t off = static_cast<off_t>(kChunkSize) - 50;
    auto out = folly::IOBuf::create(kChunkSize * 2);
    ASSERT_TRUE(rw.Read(kChunkSize, off, out.get()).ok());
    std::string expected = Repeat('A', 50) + Repeat('B', 200) +
                           std::string(kChunkSize - 250, '\0');
    EXPECT_EQ(out->length(), kChunkSize);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              expected);
  });
}

TEST_F(FileReadWriterTest, CrossChunkSecondChunkMissing) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());

    off_t off = static_cast<off_t>(kChunkSize) - 50;
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(200, off, out.get()).ok());
    std::string expected = Repeat('A', 50) + std::string(150, '\0');
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              expected);
  });
}

// ────────────────────────────────────────────────────────────────
// Edge cases
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, ZeroSizeRequest) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(0, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, EmptyOutputOnNoData) {
  RunInTestFiber([&] {
    auto rw = Make();
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(64, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 64);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              std::string(64, '\0'));
  });
}

TEST_F(FileReadWriterTest, SparseReadWithMultipleHoles) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', kChunkSize)),
                         static_cast<off_t>(kChunkSize * 3))
                    .ok());

    auto out = folly::IOBuf::create(kChunkSize * 4);
    ASSERT_TRUE(rw.Read(kChunkSize * 4, 0, out.get()).ok());
    std::string expected =
        Repeat('A', kChunkSize) +
        std::string(kChunkSize * 2, '\0') +
        Repeat('B', kChunkSize);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              expected);
  });
}

// ────────────────────────────────────────────────────────────────
// Shared FileReadWriter across file handles
// ────────────────────────────────────────────────────────────────
//
// FileHandleManager ensures that two open() calls for the same inode
// share a single FileReadWriter instance.  These tests verify that
// writes through one handle are visible when reading through another.

TEST_F(FileReadWriterTest, UnflushedWriteVisibleAcrossHandles) {
  RunInTestFiber([&] {
    uint64_t fh1 = 0, fh2 = 0;
    auto &mgr = swordfs::vfs::FileHandleManager::Instance();
    swordfs::vfs::FileHandle opened1, opened2;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened1).ok());
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened2).ok());
    fh1 = opened1.fh();
    fh2 = opened2.fh();

    auto h1 = mgr.Find(fh1);
    auto h2 = mgr.Find(fh2);
    ASSERT_TRUE(h1.has_value());
    ASSERT_TRUE(h2.has_value());
    EXPECT_EQ(h1->handle().get(), h2->handle().get());

    // Write through handle 1, read through handle 2 (same instance).
    ASSERT_TRUE(h1->Write(Buf(Repeat('Z', 300)), 100).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(h2->Read(300, 100, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('Z', 300));

    mgr.Release(fh1);
    mgr.Release(fh2);
  });
}

// ────────────────────────────────────────────────────────────────
// Flushed data visible across handles (same FileReadWriter)
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FlushedDataVisibleAcrossHandles) {
  RunInTestFiber([&] {
    uint64_t fh1 = 0, fh2 = 0;
    auto &mgr = swordfs::vfs::FileHandleManager::Instance();
    swordfs::vfs::FileHandle opened1, opened2;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened1).ok());
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened2).ok());
    fh1 = opened1.fh();
    fh2 = opened2.fh();

    auto h1 = mgr.Find(fh1);
    auto h2 = mgr.Find(fh2);

    // Write + flush through handle 1.
    ASSERT_TRUE(h1->Write(Buf(Repeat('X', 500)), 0).ok());
    ASSERT_TRUE(h1->Flush().ok());

    // Read through handle 2 — same instance, flushed chunk in map.
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(h2->Read(500, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('X', 500));

    mgr.Release(fh1);
    mgr.Release(fh2);
  });
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, TruncateCallsMetaEngine) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Truncate(1024).ok());
    EXPECT_EQ(mock_meta_->truncate_calls, 1);
    EXPECT_EQ(mock_meta_->file_size(), 1024);
  });
}

TEST_F(FileReadWriterTest, TruncatePropagatesMetaError) {
  RunInTestFiber([&] {
    mock_meta_->set_truncate_status(Status::Internal("truncate failed"));
    auto rw = Make();
    auto status = rw.Truncate(1024);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), Status::kInternal);
  });
}

TEST_F(FileReadWriterTest, TruncateDropsDirtyChunks) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());

    // Truncating to zero must drop the dirty chunk so a later read
    // returns zeros instead of the previously written data.
    ASSERT_TRUE(rw.Truncate(0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(16, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              std::string(16, '\0'));
  });
}

TEST_F(FileReadWriterTest, TruncateDeletesDroppedChunkObjects) {
  // Truncating a multi-chunk file must ask the data engine to delete
  // the chunk objects that no longer fit the new size. Without this,
  // S3 objects would leak until a future GC pass picks them up.
  //
  // The default VolumeImpl chunk_size is 64 MiB, so seeding multiple
  // chunks via the high-level Write API would require writing >64 MiB
  // per chunk. Use the test-only chunk-size escape hatch on the volume
  // singleton to shrink it for the duration of this test.
  RunInTestFiber([&] {
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_chunk_size_for_test(kChunkSize);

    auto rw = Make();

    // Register three chunks with the mock metadata engine and seed
    // them in the data engine so the truncate-side Delete has
    // something to act on.
    for (ChunkIndex i = 0; i < 3; ++i) {
      ChunkMeta cm{};
      cm.index = i;
      cm.start_offset = i * kChunkSize;
      cm.key = std::to_string(kIno) + "/" + std::to_string(i);
      cm.size = kChunkSize;
      ASSERT_TRUE(mock_meta_->AddChunk(kIno, cm).ok());
      auto buf = std::make_unique<folly::IOBuf>(Buf(Repeat('A', kChunkSize)));
      ASSERT_TRUE(mock_data_->Put(cm.key, std::move(buf)).ok());
    }
    // Materialise each chunk in FileChunkManager by reading it; the
    // reader path uses chunks_.Get(idx, false) which still triggers
    // Chunk::Initialize → meta->FindChunk → state kFlushed.
    for (ChunkIndex i = 0; i < 3; ++i) {
      auto out = folly::IOBuf::create(kChunkSize);
      ASSERT_TRUE(rw.Read(kChunkSize, i * kChunkSize, out.get()).ok());
    }
    ASSERT_EQ(mock_data_->StoredKeys().size(), 3);

    // Truncate to one byte — chunks 1 and 2 are dropped (their data
    // no longer fits the new size), chunk 0 is kept alive.
    ASSERT_TRUE(rw.Truncate(1).ok());

    std::sort(mock_data_->delete_calls.begin(),
              mock_data_->delete_calls.end());
    EXPECT_EQ(mock_data_->delete_calls.size(), 2);
    EXPECT_EQ(mock_data_->delete_calls[0],
              std::to_string(kIno) + "/1");
    EXPECT_EQ(mock_data_->delete_calls[1],
              std::to_string(kIno) + "/2");
    EXPECT_EQ(mock_data_->StoredKeys().size(), 1);
    EXPECT_EQ(mock_data_->StoredKeys()[0],
              std::to_string(kIno) + "/0");

    // Restore the production chunk size so a later test in this
    // fixture doesn't observe the override.
    vol.clear_chunk_size_for_test();
  });
}

TEST_F(FileReadWriterTest, TruncateToleratesDataDeleteFailure) {
  // A failing per-chunk Delete must not poison the Truncate result;
  // the metadata side already committed the new size.
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', kChunkSize * 2)), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    mock_data_->delete_status = Status::Internal("forced");

    EXPECT_TRUE(rw.Truncate(0).ok());
    // The metadata side dropped the chunks even though the data engine
    // refused: that's the documented best-effort contract.
    EXPECT_EQ(mock_meta_->truncate_calls, 1);
    EXPECT_EQ(mock_meta_->file_size(), 0);
  });
}
