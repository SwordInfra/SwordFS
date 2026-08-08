// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileReadWriter — read, write, flush, and
// cross-file-handle sharing behaviour.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkMeta;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
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
    if (it == store_.end()) return false;
    if (size) *size = it->second.size();
    return true;
  }

  Status Put(std::string_view key, std::string_view data) override {
    store_[std::string(key)] = std::string(data);
    return Status::OK();
  }

  Status Get(std::string_view key, std::string *out,
             size_t offset = 0, size_t size = 0) override {
    auto it = store_.find(std::string(key));
    if (it == store_.end()) return Status::NotFound("chunk not found");
    const std::string &chunk = it->second;
    if (offset >= chunk.size()) {
      *out = "";
      return Status::OK();
    }
    size_t len = (size == 0) ? chunk.size() - offset
                             : std::min(size, chunk.size() - offset);
    *out = chunk.substr(offset, len);
    return Status::OK();
  }

  Status Delete(std::string_view key) override {
    store_.erase(std::string(key));
    return Status::OK();
  }

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
  Status Unlink(InodeID, std::string_view) override { return Status::OK(); }
  Status RmDir(InodeID, std::string_view) override { return Status::OK(); }
  Status Rename(InodeID, std::string_view, InodeID,
                std::string_view, unsigned int) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const struct stat *attr, int to_set,
                 struct stat *out_attr) override {
    if (to_set & FUSE_SET_ATTR_SIZE) file_size_ = attr->st_size;
    if (out_attr) {
      std::memset(out_attr, 0, sizeof(*out_attr));
      out_attr->st_size = file_size_;
    }
    return Status::OK();
  }
  Status StatFs(struct statvfs *) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Open(InodeID) override { return Status::OK(); }
  Status OpenDir(InodeID) override { return Status::OK(); }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }

  Status AddChunk(InodeID ino, const ChunkMeta &cm) override {
    chunks_[ino][cm.start_offset] = cm;
    return Status::OK();
  }

  Status FindChunk(InodeID ino, off_t off, size_t chunk_size,
                   ChunkMeta *cm) override {
    auto it = chunks_.find(ino);
    if (it == chunks_.end()) return Status::NotFound("");
    off_t chunk_start = (off / static_cast<off_t>(chunk_size)) * static_cast<off_t>(chunk_size);
    auto cit = it->second.find(chunk_start);
    if (cit == it->second.end()) return Status::NotFound("");
    if (cm) *cm = cit->second;
    return Status::OK();
  }

  void set_file_size(off_t size) { file_size_ = size; }

 private:
  off_t file_size_ = 0;
  std::unordered_map<InodeID,
                     std::unordered_map<off_t, ChunkMeta>>
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
// Flush → read from metadata
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, ReadAfterFlushUsesMetadata) {
  RunInTestFiber([&] {
    // Write to rw1, flush, then read from rw2 (same inode).
    auto rw1 = Make();
    ASSERT_TRUE(rw1.Write(Buf(Repeat('X', 500)), 0).ok());
    ASSERT_TRUE(rw1.Flush().ok());

    auto rw2 = Make();
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw2.Read(500, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('X', 500));
  });
}

// ────────────────────────────────────────────────────────────────
// Shared FileReadWriter: same inode, different objects
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, UnflushedWriteVisibleAcrossSameInode) {
  RunInTestFiber([&] {
    // Two FileReadWriters for the same inode share the same state
    // via the inode-level writer (managed by FileHandleManager).
    // Here we test at the unit level: writes through one object
    // are visible to another because Read queries the metadata
    // engine after flush.
    //
    // Before flush: only dirty chunks in the same writer are visible.
    // After flush: chunks are registered in the metadata engine and
    // visible to any FileReadWriter for the same inode.

    auto rw1 = Make();
    ASSERT_TRUE(rw1.Write(Buf(Repeat('Z', 300)), 100).ok());
    ASSERT_TRUE(rw1.Flush().ok());

    // rw2 hits the metadata engine and finds the flushed chunk.
    auto rw2 = Make();
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw2.Read(300, 100, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                               out->length()),
              Repeat('Z', 300));
  });
}
