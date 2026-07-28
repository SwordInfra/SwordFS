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
    lim.max_chunk_size = chunk_size_;
    lim.supports_multipart = false;
    return lim;
  }

  void set_chunk_size(size_t sz) { chunk_size_ = sz; }

  bool Head(std::string_view key, size_t* size) override {
    auto it = store_.find(std::string(key));
    if (it == store_.end()) return false;
    if (size) *size = it->second.size();
    return true;
  }

  Status Put(std::string_view key, std::string_view data) override {
    store_[std::string(key)] = std::string(data);
    return Status::OK();
  }

  Status Get(std::string_view key, std::string* out,
             size_t offset = 0, size_t size = 0) override {
    auto it = store_.find(std::string(key));
    if (it == store_.end()) return Status::NotFound("chunk not found");

    const std::string& chunk = it->second;
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

  void InjectChunk(const std::string& key, std::string data) {
    store_[key] = std::move(data);
  }

  bool has_chunk(const std::string& key) const {
    return store_.count(key) > 0;
  }

  std::string get_chunk_data(const std::string& key) const {
    auto it = store_.find(key);
    return (it != store_.end()) ? it->second : "";
  }

  size_t chunk_count() const { return store_.size(); }

 private:
  size_t chunk_size_ = 64 * 1024 * 1024;
  std::unordered_map<std::string, std::string> store_;
};

// ================================================================
// MockMetaEngine — minimal IMetaEngine for ChunkManager tests.
// ================================================================

class MockMetaEngine : public IMetaEngine {
 public:
  Status Lookup(InodeID, std::string_view, InodeID*,
                struct stat*) override {
    return Status::OK();
  }
  Status GetAttr(InodeID, struct stat* attr) override {
    std::memset(attr, 0, sizeof(*attr));
    return Status::OK();
  }
  Status ReadDir(InodeID,
                 std::vector<swordfs::metadata::SwordFsEntry>*) override {
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, mode_t, InodeID*,
                struct stat*) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID*,
               struct stat*) override {
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
  Status SetAttr(InodeID, const struct stat*, int,
                 struct stat*) override {
    return Status::OK();
  }
  Status StatFs(struct statvfs*) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Open(InodeID, uint64_t* fh) override {
    *fh = 1;
    return Status::OK();
  }
  Status Release(uint64_t) override { return Status::OK(); }
  Status OpenDir(InodeID, uint64_t* fh) override {
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
    mock_data_->set_chunk_size(kChunkSize);
    mock_meta_ = std::make_unique<MockMetaEngine>();
    chunk_mgr_ = std::make_unique<ChunkManager>(mock_meta_.get(),
                                                mock_data_.get());
  }

  void ExpectRead(InodeID ino, size_t size, off_t off,
                  const std::string& expected) {
    std::string out;
    Status st = chunk_mgr_->Read(ino, size, off, &out);
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(out, expected);
  }

  static constexpr size_t kChunkSize = 1024;
  static constexpr uint64_t kFh = 1;
  std::unique_ptr<MockDataEngine> mock_data_;
  std::unique_ptr<MockMetaEngine> mock_meta_;
  std::unique_ptr<ChunkManager> chunk_mgr_;
};

// ────────────────────────────────────────────────────────────────
// Read — basic single-chunk reads
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, FullChunk) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, kChunkSize, 0, Repeat('A', kChunkSize));
}

TEST_F(ChunkManagerReadTest, WithinChunkWithOffset) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, 100, 200, Repeat('A', 100));
}

TEST_F(ChunkManagerReadTest, PartialChunkAtEOF) {
  mock_data_->InjectChunk("42/0", Repeat('A', 500));
  ExpectRead(42, kChunkSize, 0, Repeat('A', 500));
}

TEST_F(ChunkManagerReadTest, PastEOF) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, 100, kChunkSize + 100, "");
}

// ────────────────────────────────────────────────────────────────
// Read — cross-chunk reads
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, CrossChunkBoundary) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_data_->InjectChunk("42/1", Repeat('B', 500));

  off_t off = static_cast<off_t>(kChunkSize) - 100;
  std::string expected = Repeat('A', 100) + Repeat('B', 500);
  ExpectRead(42, 600, off, expected);
}

TEST_F(ChunkManagerReadTest, CrossChunkExactBoundary) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_data_->InjectChunk("42/1", Repeat('B', 500));

  ExpectRead(42, 500, kChunkSize, Repeat('B', 500));
}

TEST_F(ChunkManagerReadTest, CrossChunkReadsIntoSecondChunk) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_data_->InjectChunk("42/1", Repeat('B', 800));

  off_t off = static_cast<off_t>(kChunkSize) - 1;
  std::string expected = "A" + Repeat('B', 500);
  ExpectRead(42, 501, off, expected);
}

TEST_F(ChunkManagerReadTest, CrossChunkExhaustsSecondChunk) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_data_->InjectChunk("42/1", Repeat('B', 200));

  off_t off = static_cast<off_t>(kChunkSize) - 50;
  std::string expected = Repeat('A', 50) + Repeat('B', 200);
  ExpectRead(42, 1000, off, expected);
}

TEST_F(ChunkManagerReadTest, CrossChunkSecondChunkMissing) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));

  off_t off = static_cast<off_t>(kChunkSize) - 50;
  ExpectRead(42, 1000, off, Repeat('A', 50));
}

// ────────────────────────────────────────────────────────────────
// Read — edge cases
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, ZeroSizeRequest) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, 0, 0, "");
}

TEST_F(ChunkManagerReadTest, NoDataEngine) {
  ChunkManager no_data(mock_meta_.get(), nullptr);
  std::string out;
  Status st = no_data.Read(42, 100, 0, &out);
  EXPECT_FALSE(st.ok());
}

TEST_F(ChunkManagerReadTest, EmptyOutputOnNonexistentChunk) {
  ExpectRead(42, 100, 0, "");
}

// ────────────────────────────────────────────────────────────────
// Read — write-buffer awareness (unflushed data)
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerReadTest, ReadFromUnflushedBufferOnly) {
  // Write data to buffer (all fits in one chunk, no auto-flush).
  Status st = chunk_mgr_->Write(kFh, 42, Repeat('X', 500).data(), 500, 0);
  EXPECT_TRUE(st.ok());
  ExpectRead(42, 500, 0, Repeat('X', 500));
}

TEST_F(ChunkManagerReadTest, ReadFromUnflushedBufferWithOffset) {
  chunk_mgr_->Write(kFh, 42, Repeat('X', 500).data(), 500, 0);
  ExpectRead(42, 200, 100, Repeat('X', 200));
}

TEST_F(ChunkManagerReadTest, ReadPastEndOfBufferToStorage) {
  // Chunk 0 flushed to storage, chunk 1 partially in buffer.
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  // Write chunk 0 worth first → auto-flushes to storage.
  chunk_mgr_->Write(kFh, 42, Repeat('B', kChunkSize).data(), kChunkSize, 0);
  // Now write partial chunk 1 → stays in buffer.
  chunk_mgr_->Write(kFh, 42, Repeat('B', 300).data(), 300, kChunkSize);

  // Read starting in chunk 0, spanning into buffer.
  std::string expected =
      Repeat('B', kChunkSize - 900) +
      Repeat('B', 400 - (kChunkSize - 900));
  ExpectRead(42, 400, 900, expected);
}

TEST_F(ChunkManagerReadTest, ReadBufferTakesPrecedenceOverStorage) {
  mock_data_->InjectChunk("42/0", Repeat('S', kChunkSize));
  // Write to buffer — same offset range as chunk 0.
  chunk_mgr_->Write(kFh, 42, Repeat('B', 200).data(), 200, 0);

  // Buffer data should take precedence.
  std::string expected = Repeat('B', 200) + Repeat('S', 100);
  ExpectRead(42, 300, 0, expected);
}

TEST_F(ChunkManagerReadTest, ReadEntirelyBeforeBuffer) {
  mock_data_->InjectChunk("42/0", Repeat('A', kChunkSize));
  // Write chunk 0 → auto-flushes, then write partial chunk 1.
  chunk_mgr_->Write(kFh, 42, Repeat('B', kChunkSize).data(), kChunkSize, 0);
  chunk_mgr_->Write(kFh, 42, Repeat('B', 200).data(), 200, kChunkSize);

  // Read entirely within chunk 0 — comes from storage.
  ExpectRead(42, 100, 500, Repeat('B', 100));
}

// ================================================================
// ChunkManagerFlushTest
// ================================================================

class ChunkManagerFlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_data_ = std::make_unique<MockDataEngine>();
    mock_data_->set_chunk_size(kChunkSize);
    mock_meta_ = std::make_unique<MockMetaEngine>();
    chunk_mgr_ = std::make_unique<ChunkManager>(mock_meta_.get(),
                                                mock_data_.get());
  }

  static constexpr size_t kChunkSize = 256;
  static constexpr uint64_t kFh = 1;
  static constexpr InodeID kIno = 42;

  std::unique_ptr<MockDataEngine> mock_data_;
  std::unique_ptr<MockMetaEngine> mock_meta_;
  std::unique_ptr<ChunkManager> chunk_mgr_;
};

// ────────────────────────────────────────────────────────────────
// Flush — non-force (auto) flush
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerFlushTest, NoBuffer) {
  Status st = chunk_mgr_->Flush(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 0);
}

TEST_F(ChunkManagerFlushTest, BelowThresholdNoFlush) {
  chunk_mgr_->Write(kFh, kIno, Repeat('X', kChunkSize - 1).data(),
                    kChunkSize - 1, 0);

  Status st = chunk_mgr_->Flush(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 0);

  // Read back from buffer to verify data is still there.
  std::string out;
  chunk_mgr_->Read(kIno, kChunkSize - 1, 0, &out);
  EXPECT_EQ(out, Repeat('X', kChunkSize - 1));
}

TEST_F(ChunkManagerFlushTest, AtThresholdFlushExactlyOneChunk) {
  chunk_mgr_->Write(kFh, kIno, Repeat('X', kChunkSize).data(),
                    kChunkSize, 0);

  // Write at threshold triggers auto-flush internally.
  EXPECT_EQ(mock_data_->chunk_count(), 1);
  EXPECT_TRUE(mock_data_->has_chunk("42/0"));
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('X', kChunkSize));
}

TEST_F(ChunkManagerFlushTest, AboveThresholdFlushKeepsRemainder) {
  // Write 1.5 chunks — first chunk auto-flushes, remainder stays.
  size_t total = kChunkSize + kChunkSize / 2;
  chunk_mgr_->Write(kFh, kIno, Repeat('X', total).data(), total, 0);

  // Chunk 0 should have been auto-flushed.
  EXPECT_EQ(mock_data_->get_chunk_data("42/0").size(), kChunkSize);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('X', kChunkSize));

  // Remainder is still in buffer — readable.
  std::string out;
  chunk_mgr_->Read(kIno, kChunkSize / 2, kChunkSize, &out);
  EXPECT_EQ(out, Repeat('X', kChunkSize / 2));
}

TEST_F(ChunkManagerFlushTest, MultipleNonForceFlushes) {
  // Write 3.5 chunks: 3 auto-flush, 0.5 stays in buffer.
  size_t total = 3 * kChunkSize + kChunkSize / 2;
  chunk_mgr_->Write(kFh, kIno, Repeat('X', total).data(), total, 0);

  EXPECT_EQ(mock_data_->chunk_count(), 3);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('X', kChunkSize));
  EXPECT_EQ(mock_data_->get_chunk_data("42/1"), Repeat('X', kChunkSize));
  EXPECT_EQ(mock_data_->get_chunk_data("42/2"), Repeat('X', kChunkSize));

  // Remainder readable from buffer.
  std::string out;
  chunk_mgr_->Read(kIno, kChunkSize / 2, 3 * kChunkSize, &out);
  EXPECT_EQ(out, Repeat('X', kChunkSize / 2));
}

// ────────────────────────────────────────────────────────────────
// Flush — force flush
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerFlushTest, ForceFlushEmpty) {
  Status st = chunk_mgr_->Flush(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 0);
}

TEST_F(ChunkManagerFlushTest, ForceFlushBelowThreshold) {
  chunk_mgr_->Write(kFh, kIno, Repeat('Y', kChunkSize / 2).data(),
                    kChunkSize / 2, 0);

  Status st = chunk_mgr_->Flush(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 1);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"),
            Repeat('Y', kChunkSize / 2));
}

TEST_F(ChunkManagerFlushTest, ForceFlushAboveThreshold) {
  // Write 2 full chunks + 10 bytes — first 2 auto-flush.
  chunk_mgr_->Write(kFh, kIno, Repeat('Z', kChunkSize * 2 + 10).data(),
                    kChunkSize * 2 + 10, 0);

  // Force-flush the remaining 10 bytes.
  Status st = chunk_mgr_->Flush(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 3);
  EXPECT_EQ(mock_data_->get_chunk_data("42/2"), Repeat('Z', 10));
}

// ────────────────────────────────────────────────────────────────
// Flush — chunk indexing
// ────────────────────────────────────────────────────────────────

TEST_F(ChunkManagerFlushTest, ChunkIndexIncrementsCorrectly) {
  // Write 3 full chunks — all auto-flush.
  chunk_mgr_->Write(kFh, kIno, Repeat('X', kChunkSize * 3).data(),
                    kChunkSize * 3, 0);

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

TEST_F(ChunkManagerFlushTest, DataIntegrityAcrossFlushes) {
  std::string chunk_a = Repeat('A', kChunkSize);
  std::string chunk_b = Repeat('B', kChunkSize);
  std::string leftover = Repeat('C', 10);

  // Write all at once — first 2 chunks auto-flush, 10 bytes remain.
  chunk_mgr_->Write(kFh, kIno,
                    (chunk_a + chunk_b + leftover).data(),
                    chunk_a.size() + chunk_b.size() + leftover.size(), 0);

  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), chunk_a);
  EXPECT_EQ(mock_data_->get_chunk_data("42/1"), chunk_b);

  // Force flush remainder.
  Status st = chunk_mgr_->Flush(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/2"), leftover);
}
