// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for VfsImpl::HandleRead and VfsImpl::FlushWriteBuf.
// Uses in-memory mocks so that no FUSE, S3, or filesystem dependencies
// are required.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "config/ConfigCenter.hpp"
#include "fuse/VfsImpl.hpp"
#include "metadata/Meta.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"

using swordfs::config::ConfigCenter;
using swordfs::fuse::VfsImpl;
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
// Stores chunks in a std::unordered_map.
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

  /// Directly inject a chunk for testing reads.
  void InjectChunk(const std::string& key, std::string data) {
    store_[key] = std::move(data);
  }

  /// Verify a chunk was written with expected content.
  bool has_chunk(const std::string& key) const {
    return store_.count(key) > 0;
  }

  /// Get the stored data for a chunk.
  std::string get_chunk_data(const std::string& key) const {
    auto it = store_.find(key);
    return (it != store_.end()) ? it->second : "";
  }

  /// Returns the number of chunks stored.
  size_t chunk_count() const { return store_.size(); }

 private:
  size_t chunk_size_ = 64 * 1024 * 1024;  // 64 MiB default
  std::unordered_map<std::string, std::string> store_;
};

// ================================================================
// MockMetaEngine — minimal IMetaEngine for FlushWriteBuf tests.
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
  Status ReadDir(InodeID, std::vector<swordfs::metadata::SwordFsEntry>*) override {
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
// HandleReadTest
// ================================================================

class HandleReadTest : public VfsImpl, public ::testing::Test {
 protected:
  void SetUp() override {
    ConfigCenter::Instance().set_meta_url(
        std::string{swordfs::metadata::kMemoryMetaUrl});
    ASSERT_TRUE(Init().ok());
    auto mock = std::make_unique<MockDataEngine>();
    mock_ = mock.get();
    mock_->set_chunk_size(kChunkSize);
    data_engine_ = std::move(mock);
  }

  void TearDown() override {
    write_bufs_.clear();
  }

  /// Helper: call HandleRead and assert success + expected data.
  void ExpectRead(InodeID ino, size_t size, off_t off,
                  const std::string& expected) {
    std::string out;
    Status st = HandleRead(ino, size, off, &out);
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(out, expected);
  }

  /// Simulate unflushed writes into the write buffer for |fh|.
  /// The buffer's data range starts at next_chunk * max_chunk_size.
  void BufferWrite(uint64_t fh, InodeID ino, const std::string& data) {
    auto& wb = write_bufs_[fh];
    if (wb.ino == 0) {
      wb.ino = ino;
      wb.max_chunk_size = mock_->Limits().max_chunk_size;
    }
    wb.data.insert(wb.data.end(), data.begin(), data.end());
    off_t end = static_cast<off_t>(wb.next_chunk) *
                    static_cast<off_t>(wb.max_chunk_size) +
                static_cast<off_t>(wb.data.size());
    if (end > wb.max_write_end) wb.max_write_end = end;
  }

  static constexpr size_t kChunkSize = 1024;
  static constexpr uint64_t kFh = 1;
  MockDataEngine* mock_ = nullptr;
};

// ────────────────────────────────────────────────────────────────
// HandleRead — basic single-chunk reads
// ────────────────────────────────────────────────────────────────

TEST_F(HandleReadTest, FullChunk) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, kChunkSize, 0, Repeat('A', kChunkSize));
}

TEST_F(HandleReadTest, WithinChunkWithOffset) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  // Read 100 bytes starting at offset 200.
  ExpectRead(42, 100, 200, Repeat('A', 100));
}

TEST_F(HandleReadTest, PartialChunkAtEOF) {
  // Chunk 0 only has 500 bytes (smaller than kChunkSize).
  mock_->InjectChunk("42/0", Repeat('A', 500));
  // Request more than available — should return only what exists.
  ExpectRead(42, kChunkSize, 0, Repeat('A', 500));
}

TEST_F(HandleReadTest, PastEOF) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, 100, kChunkSize + 100, "");
}

// ────────────────────────────────────────────────────────────────
// HandleRead — cross-chunk reads (the critical regression case)
// ────────────────────────────────────────────────────────────────

TEST_F(HandleReadTest, CrossChunkBoundary) {
  // FUSE requires exactly |size| bytes; a read that starts in chunk 0
  // and extends into chunk 1 MUST return concatenated data, not a
  // short read that the kernel would zero-pad.
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_->InjectChunk("42/1", Repeat('B', 500));

  // Start 100 bytes before chunk boundary, request 600 bytes:
  //   100 bytes of 'A' + 500 bytes of 'B' = 600 total.
  off_t off = static_cast<off_t>(kChunkSize) - 100;
  std::string expected = Repeat('A', 100) + Repeat('B', 500);
  ExpectRead(42, 600, off, expected);
}

TEST_F(HandleReadTest, CrossChunkExactBoundary) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_->InjectChunk("42/1", Repeat('B', 500));

  // Start exactly at the chunk boundary.
  ExpectRead(42, 500, kChunkSize, Repeat('B', 500));
}

TEST_F(HandleReadTest, CrossChunkReadsIntoSecondChunk) {
  // Only the last byte of chunk 0, then deep into chunk 1.
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_->InjectChunk("42/1", Repeat('B', 800));

  off_t off = static_cast<off_t>(kChunkSize) - 1;
  std::string expected = "A" + Repeat('B', 500);
  ExpectRead(42, 501, off, expected);
}

TEST_F(HandleReadTest, CrossChunkExhaustsSecondChunk) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  mock_->InjectChunk("42/1", Repeat('B', 200));

  // Request more than available across the boundary.
  off_t off = static_cast<off_t>(kChunkSize) - 50;
  std::string expected = Repeat('A', 50) + Repeat('B', 200);
  ExpectRead(42, 1000, off, expected);
}

TEST_F(HandleReadTest, CrossChunkSecondChunkMissing) {
  // Only chunk 0 exists; chunk 1 is absent.
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));

  off_t off = static_cast<off_t>(kChunkSize) - 50;
  // Should get the 50 bytes from chunk 0 and stop at EOF.
  ExpectRead(42, 1000, off, Repeat('A', 50));
}

// ────────────────────────────────────────────────────────────────
// HandleRead — edge cases
// ────────────────────────────────────────────────────────────────

TEST_F(HandleReadTest, ZeroSizeRequest) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  ExpectRead(42, 0, 0, "");
}

TEST_F(HandleReadTest, NoDataEngine) {
  data_engine_.reset();
  std::string out;
  Status st = HandleRead(42, 100, 0, &out);
  EXPECT_FALSE(st.ok());
}

TEST_F(HandleReadTest, EmptyOutputOnNonexistentChunk) {
  // No chunks injected at all.
  ExpectRead(42, 100, 0, "");
}

// ────────────────────────────────────────────────────────────────
// HandleRead — write-buffer awareness (unflushed data)
// ────────────────────────────────────────────────────────────────

TEST_F(HandleReadTest, ReadFromUnflushedBufferOnly) {
  // Data exists only in the write buffer, not in storage.
  // Buffer at chunk 0 (next_chunk=0), so offsets [0, 500).
  BufferWrite(kFh, 42, Repeat('X', 500));
  ExpectRead(42, 500, 0, Repeat('X', 500));
}

TEST_F(HandleReadTest, ReadFromUnflushedBufferWithOffset) {
  BufferWrite(kFh, 42, Repeat('X', 500));
  // Read starting from offset 100 within the buffer.
  ExpectRead(42, 200, 100, Repeat('X', 200));
}

TEST_F(HandleReadTest, ReadPastEndOfBufferToStorage) {
  // Chunk 0 flushed to storage, chunk 1 partially in buffer.
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  // Simulate: chunk 0 already flushed, unflushed data at chunk 1 position.
  {
    auto& wb = write_bufs_[kFh];
    wb.ino = 42;
    wb.max_chunk_size = kChunkSize;
    wb.next_chunk = 1;  // buffer data starts at offset 1024
    std::string buf_data = Repeat('B', 300);
    wb.data.assign(buf_data.begin(), buf_data.end());
  }
  // Read starting in chunk 0, spanning into buffer: offsets [900, 1300).
  // 124 bytes from storage (chunk 0) + 276 bytes from buffer = 400 total.
  std::string expected =
      Repeat('A', kChunkSize - 900) + Repeat('B', 400 - (kChunkSize - 900));
  ExpectRead(42, 400, 900, expected);
}

TEST_F(HandleReadTest, ReadBufferTakesPrecedenceOverStorage) {
  // Same offset has data in BOTH storage and buffer — buffer wins.
  mock_->InjectChunk("42/0", Repeat('S', kChunkSize));  // S = storage
  BufferWrite(kFh, 42, Repeat('B', 200));               // B = buffer

  // Read first 300 bytes; first 200 should come from buffer.
  std::string expected = Repeat('B', 200) + Repeat('S', 100);
  ExpectRead(42, 300, 0, expected);
}

TEST_F(HandleReadTest, ReadEntirelyBeforeBuffer) {
  mock_->InjectChunk("42/0", Repeat('A', kChunkSize));
  // Buffer at chunk 1 position (offsets [1024, ...)).
  {
    auto& wb = write_bufs_[kFh];
    wb.ino = 42;
    wb.max_chunk_size = kChunkSize;
    wb.next_chunk = 1;
    std::string buf_data = Repeat('B', 200);
    wb.data.assign(buf_data.begin(), buf_data.end());
  }
  // Read entirely within chunk 0 — should come from storage.
  ExpectRead(42, 100, 500, Repeat('A', 100));
}

// ================================================================
// FlushWriteBufTest
// ================================================================

class FlushWriteBufTest : public VfsImpl, public ::testing::Test {
 protected:
  void SetUp() override {
    ConfigCenter::Instance().set_meta_url(
        std::string{swordfs::metadata::kMemoryMetaUrl});
    ASSERT_TRUE(Init().ok());
    auto mock_data = std::make_unique<MockDataEngine>();
    mock_data_ = mock_data.get();
    mock_data_->set_chunk_size(kChunkSize);
    data_engine_ = std::move(mock_data);

    meta_engine_ = std::make_unique<MockMetaEngine>();
  }

  /// Simulate a write to file handle |fh| by injecting data directly
  /// into the write buffer.
  void BufferWrite(uint64_t fh, InodeID ino, const std::string& data) {
    auto& wb = write_bufs_[fh];
    if (wb.ino == 0) {
      wb.ino = ino;
      wb.max_chunk_size = mock_data_->Limits().max_chunk_size;
    }
    wb.data.insert(wb.data.end(), data.begin(), data.end());
    off_t end = static_cast<off_t>(wb.data.size());
    if (end > wb.max_write_end) wb.max_write_end = end;
  }

  static constexpr size_t kChunkSize = 256;
  static constexpr uint64_t kFh = 1;
  static constexpr InodeID kIno = 42;

  MockDataEngine* mock_data_ = nullptr;
};

// ────────────────────────────────────────────────────────────────
// FlushWriteBuf — non-force (auto) flush
// ────────────────────────────────────────────────────────────────

TEST_F(FlushWriteBufTest, NoBuffer) {
  Status st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 0);
}

TEST_F(FlushWriteBufTest, BelowThresholdNoFlush) {
  BufferWrite(kFh, kIno, Repeat('X', kChunkSize - 1));
  EXPECT_EQ(write_bufs_[kFh].data.size(), kChunkSize - 1);

  Status st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  // Buffer should be unchanged — nothing flushed.
  EXPECT_EQ(write_bufs_[kFh].data.size(), kChunkSize - 1);
  EXPECT_EQ(mock_data_->chunk_count(), 0);
}

TEST_F(FlushWriteBufTest, AtThresholdFlushExactlyOneChunk) {
  BufferWrite(kFh, kIno, Repeat('X', kChunkSize));
  EXPECT_EQ(write_bufs_[kFh].data.size(), kChunkSize);

  Status st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  // Buffer should be empty after flush of exactly max_chunk_size.
  EXPECT_TRUE(write_bufs_[kFh].data.empty());
  EXPECT_EQ(mock_data_->chunk_count(), 1);
  EXPECT_TRUE(mock_data_->has_chunk("42/0"));
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('X', kChunkSize));
  EXPECT_EQ(write_bufs_[kFh].next_chunk, 1);
}

TEST_F(FlushWriteBufTest, AboveThresholdFlushKeepsRemainder) {
  // Write 1.5 chunks worth of data.
  size_t total = kChunkSize + kChunkSize / 2;
  BufferWrite(kFh, kIno, Repeat('X', total));

  Status st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());

  // Chunk 0 should contain exactly kChunkSize bytes.
  EXPECT_EQ(mock_data_->get_chunk_data("42/0").size(), kChunkSize);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('X', kChunkSize));

  // Remainder stays in buffer for next chunk.
  EXPECT_EQ(write_bufs_[kFh].data.size(), kChunkSize / 2);
  EXPECT_EQ(write_bufs_[kFh].next_chunk, 1);
}

TEST_F(FlushWriteBufTest, MultipleNonForceFlushes) {
  // Write 3.5 chunks worth of data, trigger flush after each chunk.
  size_t total = 3 * kChunkSize + kChunkSize / 2;
  BufferWrite(kFh, kIno, Repeat('X', total));

  // First flush: chunk 0 = kChunkSize bytes.
  Status st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/0").size(), kChunkSize);
  EXPECT_EQ(write_bufs_[kFh].data.size(), total - kChunkSize);
  EXPECT_EQ(write_bufs_[kFh].next_chunk, 1);

  // Second flush: chunk 1 = kChunkSize bytes.
  st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/1").size(), kChunkSize);
  EXPECT_EQ(write_bufs_[kFh].data.size(), total - 2 * kChunkSize);
  EXPECT_EQ(write_bufs_[kFh].next_chunk, 2);

  // Third flush: chunk 2 = kChunkSize bytes.
  st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/2").size(), kChunkSize);
  // Remainder = 0.5 chunks.
  EXPECT_EQ(write_bufs_[kFh].data.size(), kChunkSize / 2);
  EXPECT_EQ(write_bufs_[kFh].next_chunk, 3);
}

// ────────────────────────────────────────────────────────────────
// FlushWriteBuf — force flush
// ────────────────────────────────────────────────────────────────

TEST_F(FlushWriteBufTest, ForceFlushEmpty) {
  Status st = FlushWriteBuf(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->chunk_count(), 0);
}

TEST_F(FlushWriteBufTest, ForceFlushBelowThreshold) {
  // Force-flush even when buffer is below max_chunk_size.
  BufferWrite(kFh, kIno, Repeat('Y', kChunkSize / 2));

  Status st = FlushWriteBuf(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_TRUE(write_bufs_[kFh].data.empty());
  EXPECT_EQ(mock_data_->chunk_count(), 1);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), Repeat('Y', kChunkSize / 2));
}

TEST_F(FlushWriteBufTest, ForceFlushAboveThreshold) {
  BufferWrite(kFh, kIno, Repeat('Z', kChunkSize * 2 + 10));

  Status st = FlushWriteBuf(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  // Force flush uploads ALL remaining data as one chunk.
  EXPECT_TRUE(write_bufs_[kFh].data.empty());
  EXPECT_EQ(mock_data_->chunk_count(), 1);
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"),
            Repeat('Z', kChunkSize * 2 + 10));
}

// ────────────────────────────────────────────────────────────────
// FlushWriteBuf — chunk indexing
// ────────────────────────────────────────────────────────────────

TEST_F(FlushWriteBufTest, ChunkIndexIncrementsCorrectly) {
  // Flush 3 full chunks.
  BufferWrite(kFh, kIno, Repeat('X', kChunkSize * 3));

  for (int i = 0; i < 3; i++) {
    Status st = FlushWriteBuf(kFh, /*force=*/false);
    EXPECT_TRUE(st.ok());
    std::string expected_key =
        std::to_string(kIno) + "/" + std::to_string(i);
    EXPECT_TRUE(mock_data_->has_chunk(expected_key))
        << "Missing chunk " << expected_key;
  }
  EXPECT_TRUE(write_bufs_[kFh].data.empty());
}

// ────────────────────────────────────────────────────────────────
// FlushWriteBuf — data integrity
// ────────────────────────────────────────────────────────────────

TEST_F(FlushWriteBufTest, DataIntegrityAcrossFlushes) {
  // Write alternating patterns, flush, and verify.
  std::string chunk_a = Repeat('A', kChunkSize);
  std::string chunk_b = Repeat('B', kChunkSize);
  std::string leftover = Repeat('C', 10);

  BufferWrite(kFh, kIno, chunk_a + chunk_b + leftover);

  // Flush chunk 0 → "AAA..."
  Status st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/0"), chunk_a);

  // Flush chunk 1 → "BBB..."
  st = FlushWriteBuf(kFh, /*force=*/false);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/1"), chunk_b);

  // Remainder = "CCC..."
  EXPECT_EQ(write_bufs_[kFh].data.size(), 10);

  // Force flush remainder.
  st = FlushWriteBuf(kFh, /*force=*/true);
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(mock_data_->get_chunk_data("42/2"), leftover);
}
