// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for ChunkManager.  Uses in-memory mocks so that no FUSE,
// S3, or filesystem dependencies are required.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "chunk/ChunkManager.hpp"
#include "metadata/Meta.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandleManager.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::chunk::ChunkManager;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::utils::Status;

// Helper: create a string of |n| copies of character |c|.
static std::string Repeat(char c, size_t n) {
  return std::string(n, c);
}

// ================================================================
// MockDataEngine — in-memory IDataEngine for unit testing.
// ================================================================

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

  void InjectChunk(const std::string &key, std::string data) {
    store_[key] = std::move(data);
  }

  bool has_chunk(const std::string &key) const {
    return store_.count(key) > 0;
  }

  std::string get_chunk_data(const std::string &key) const {
    auto it = store_.find(key);
    return (it != store_.end()) ? it->second : "";
  }

  size_t chunk_count() const { return store_.size(); }

 private:
  std::unordered_map<std::string, std::string> store_;
};

// ================================================================
// MockMetaEngine — minimal IMetaEngine for ChunkManager tests.
// ================================================================

class MockMetaEngine : public IMetaEngine {
 public:
  Status Lookup(InodeID, std::string_view, InodeID *,
                struct stat *) override {
    return Status::OK();
  }
  Status GetAttr(InodeID, struct stat *attr) override {
    std::memset(attr, 0, sizeof(*attr));
    return Status::OK();
  }
  Status ReadDir(InodeID,
                 std::vector<swordfs::metadata::SwordFsEntry> *) override {
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, mode_t, InodeID *,
                struct stat *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID *,
               struct stat *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID,
                std::string_view, unsigned int) override {
    return Status::OK();
  }
  Status SetAttr(InodeID, const struct stat *, int,
                 struct stat *) override {
    return Status::OK();
  }
  Status StatFs(struct statvfs *) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Open(InodeID, uint64_t *fh) override {
    *fh = 1;
    return Status::OK();
  }
  Status Release(uint64_t) override { return Status::OK(); }
  Status OpenDir(InodeID, uint64_t *fh) override {
    *fh = 1;
    return Status::OK();
  }
  Status ReleaseDir(uint64_t) override { return Status::OK(); }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }
};

// ================================================================
// ChunkManagerReadTest
// ================================================================

class ChunkManagerReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_data_ = std::make_unique<MockDataEngine>();
    mock_meta_ = std::make_unique<MockMetaEngine>();
    ChunkManager::Instance().Init(mock_meta_.get(), mock_data_.get(),
                                  kChunkSize);
    // Release any lingering handle from a previous test so the singleton
    // does not leak state between tests that share the same fh.
    swordfs::vfs::FileHandleManager::Instance().Release(kFh);
  }

  void TearDown() override {
    swordfs::vfs::FileHandleManager::Instance().Release(kFh);
    ChunkManager::Instance().ResetForTesting();
  }

  static constexpr size_t kChunkSize = 1024;
  static constexpr uint64_t kFh = 1;
  std::unique_ptr<MockDataEngine> mock_data_;
  std::unique_ptr<MockMetaEngine> mock_meta_;
};

// ────────────────────────────────────────────────────────────────
// Read — basic single-chunk reads (new model: write then read)
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, FullChunk) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', kChunkSize));
}

TEST_F(ChunkManagerReadTest, WithinChunkWithOffset) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(100, 200, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', 100));
}

TEST_F(ChunkManagerReadTest, PartialChunkAtEOF) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', 500).data(), 500, 0);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', 500));
}

TEST_F(ChunkManagerReadTest, PastEOF) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(100, kChunkSize + 100, out.get()).ok());
  EXPECT_EQ(out->length(), 0);
}

// ────────────────────────────────────────────────────────────────
// Read — cross-chunk reads
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, CrossChunkBoundary) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  rw.Write(Repeat('B', 500).data(), 500, kChunkSize);

  off_t off = static_cast<off_t>(kChunkSize) - 100;
  std::string expected = Repeat('A', 100) + Repeat('B', 500);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(600, off, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            expected);
}

TEST_F(ChunkManagerReadTest, CrossChunkExactBoundary) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  rw.Write(Repeat('B', 500).data(), 500, kChunkSize);

  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(500, kChunkSize, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('B', 500));
}

TEST_F(ChunkManagerReadTest, CrossChunkReadsIntoSecondChunk) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  rw.Write(Repeat('B', 800).data(), 800, kChunkSize);

  off_t off = static_cast<off_t>(kChunkSize) - 1;
  std::string expected = "A" + Repeat('B', 500);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(501, off, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            expected);
}

TEST_F(ChunkManagerReadTest, CrossChunkExhaustsSecondChunk) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  rw.Write(Repeat('B', 200).data(), 200, kChunkSize);

  off_t off = static_cast<off_t>(kChunkSize) - 50;
  std::string expected = Repeat('A', 50) + Repeat('B', 200);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(1000, off, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            expected);
}

TEST_F(ChunkManagerReadTest, CrossChunkSecondChunkMissing) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);

  off_t off = static_cast<off_t>(kChunkSize) - 50;
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(1000, off, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', 50));
}

// ────────────────────────────────────────────────────────────────
// Read — edge cases
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, ZeroSizeRequest) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('A', kChunkSize).data(), kChunkSize, 0);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(0, 0, out.get()).ok());
  EXPECT_EQ(out->length(), 0);
}

TEST_F(ChunkManagerReadTest, EmptyOutputOnNonexistentChunk) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(100, 0, out.get()).ok());
  EXPECT_EQ(out->length(), 0);
}

// ────────────────────────────────────────────────────────────────
// Read — write-buffer awareness (unflushed data)
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, ReadFromUnflushedBufferOnly) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('X', 500).data(), 500, 0);

  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(500, 0, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('X', 500));
}

TEST_F(ChunkManagerReadTest, ReadFromUnflushedBufferWithOffset) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('X', 500).data(), 500, 0);

  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(200, 100, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('X', 200));
}

TEST_F(ChunkManagerReadTest, ReadPastEndOfBufferToStorage) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('B', kChunkSize).data(), kChunkSize, 0);
  rw.Write(Repeat('B', 300).data(), 300, kChunkSize);

  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(400, 900, out.get()).ok());
  std::string expected =
      Repeat('B', kChunkSize - 900) +
      Repeat('B', 400 - (kChunkSize - 900));
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            expected);
}

TEST_F(ChunkManagerReadTest, ReadBufferTakesPrecedenceOverStorage) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('B', 200).data(), 200, 0);

  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(300, 0, out.get()).ok());
  // Only 200 bytes written, so only 200 readable — EOF after that
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('B', 200));
}

TEST_F(ChunkManagerReadTest, ReadEntirelyBeforeBuffer) {
  swordfs::vfs::FileReadWriter rw(kFh, 42, kChunkSize);
  rw.Write(Repeat('B', kChunkSize).data(), kChunkSize, 0);
  rw.Write(Repeat('B', 200).data(), 200, kChunkSize);

  auto out = folly::IOBuf::create(kChunkSize);
  ASSERT_TRUE(rw.Read(100, 500, out.get()).ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('B', 100));
}

// ────────────────────────────────────────────────────────────────
// Read — through FileHandleManager (regression: Create path)
// Verifies Open → Find → Write → Read works through the manager.
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, OpenThenWriteReadViaHandleManager) {
  swordfs::volume::VolumeImpl vol;
  vol.set_meta_engine(std::make_unique<MockMetaEngine>());
  vol.set_data_engine(std::make_unique<MockDataEngine>());

  auto &mgr = swordfs::vfs::FileHandleManager::Instance();
  mgr.Release(kFh);  // ensure clean state
  EXPECT_TRUE(mgr.Open(kFh, &vol, 42).ok());

  auto handle = mgr.Find(kFh);
  ASSERT_NE(handle.file_readwriter, nullptr);
  handle.file_readwriter->Write(Repeat('Z', 300).data(), 300, 0);

  auto out = folly::IOBuf::create(kChunkSize);
  Status st = handle.file_readwriter->Read(300, 0, out.get());
  EXPECT_TRUE(st.ok()) << st.message();
  std::string_view sv(reinterpret_cast<const char *>(out->data()),
                      out->length());
  EXPECT_EQ(sv, Repeat('Z', 300));

  mgr.Release(kFh);
}

// ================================================================

class ChunkManagerFlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_data_ = std::make_unique<MockDataEngine>();
    mock_meta_ = std::make_unique<MockMetaEngine>();
    ChunkManager::Instance().Init(mock_meta_.get(), mock_data_.get(),
                                  kChunkSize);
  }

  void TearDown() override {
    ChunkManager::Instance().ResetForTesting();
  }

  static constexpr size_t kChunkSize = 256;
  static constexpr uint64_t kFh = 1;
  static constexpr InodeID kIno = 42;

  std::unique_ptr<MockDataEngine> mock_data_;
  std::unique_ptr<MockMetaEngine> mock_meta_;
};

// ────────────────────────────────────────────────────────────────
// Flush — basic
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerFlushTest, NoData) {
  swordfs::vfs::FileReadWriter rw(kFh, kIno, kChunkSize);
  Status st = rw.Flush();
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 0);
}

TEST_F(ChunkManagerFlushTest, FlushUploadsPartialChunk) {
  swordfs::vfs::FileReadWriter rw(kFh, kIno, kChunkSize);
  rw.Write(Repeat('Y', kChunkSize / 2).data(),
           kChunkSize / 2, 0);

  Status st = rw.Flush();
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 1);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"),
            Repeat('Y', kChunkSize / 2));
}

TEST_F(ChunkManagerFlushTest, FlushUploadsFullChunks) {
  swordfs::vfs::FileReadWriter rw(kFh, kIno, kChunkSize);
  rw.Write(Repeat('X', kChunkSize * 2).data(),
           kChunkSize * 2, 0);

  // Chunks are sealed but not uploaded until Flush.
  EXPECT_EQ(mock_data_->chunk_count(), 0);

  Status st = rw.Flush();
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 2);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('X', kChunkSize));
  EXPECT_EQ(mock_data_->get_chunk_data("42/1"), Repeat('X', kChunkSize));
}

TEST_F(ChunkManagerFlushTest, FlushUploadsFullPlusRemainder) {
  size_t total = kChunkSize * 2 + 10;
  swordfs::vfs::FileReadWriter rw(kFh, kIno, kChunkSize);
  rw.Write(Repeat('Z', total).data(), total, 0);

  Status st = rw.Flush();
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 3);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('Z', kChunkSize));
  EXPECT_EQ(mock_data_->get_chunk_data("42/1"), Repeat('Z', kChunkSize));
  EXPECT_EQ(mock_data_->get_chunk_data("42/2"), Repeat('Z', 10));
}

// ────────────────────────────────────────────────────────────────
// Flush — chunk indexing
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerFlushTest, ChunkIndexIncrementsCorrectly) {
  swordfs::vfs::FileReadWriter rw(kFh, kIno, kChunkSize);
  rw.Write(Repeat('X', kChunkSize * 3).data(),
           kChunkSize * 3, 0);
  rw.Flush();

  for (int i = 0; i < 3; i++) {
    std::string expected_key =
        std::to_string(kIno) + "/" + std::to_string(i);
    EXPECT_TRUE(mock_data_->has_chunk(expected_key))
        << "Missing chunk " << expected_key;
  }
}

// ────────────────────────────────────────────────────────────────
// Flush — data integrity
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerFlushTest, DataIntegrityAcrossChunks) {
  std::string chunk_a = Repeat('A', kChunkSize);
  std::string chunk_b = Repeat('B', kChunkSize);
  std::string leftover = Repeat('C', 10);

  std::string all = chunk_a + chunk_b + leftover;
  swordfs::vfs::FileReadWriter rw(kFh, kIno, kChunkSize);
  rw.Write(all.data(), all.size(), 0);
  rw.Flush();

  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), chunk_a);
  EXPECT_EQ(mock_data_->get_chunk_data("42/1"), chunk_b);
  EXPECT_EQ(mock_data_->get_chunk_data("42/2"), leftover);
}
