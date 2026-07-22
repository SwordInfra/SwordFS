// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// S3 integration tests — simulate the VFS Write → Flush → Read flow
// using S3DataEngine and the deterministic chunk key convention.
//
// These tests validate the data-path contract between VfsImpl and
// IDataEngine without requiring a full FUSE mount.  They use the same
// chunk key formula that VfsImpl::ChunkKey uses:
//
//   chunk_seq = file_offset / max_chunk_size
//   key       = "chunks/" + ino + "/" + chunk_seq
//
// Requires the same S3 environment variables as S3DataEngine_test.

#ifdef SWORDFS_ENABLE_S3

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "storage/IDataEngine.hpp"
#include "storage/s3/S3DataEngine.hpp"
#include "utils/Status.hpp"

using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::storage::S3Config;
using swordfs::storage::S3DataEngine;
using swordfs::utils::Status;

namespace {

// ── S3 config from environment ──────────────────────────────────────────

bool LoadConfig(S3Config* cfg) {
  const char* endpoint = std::getenv("SWORDFS_TEST_S3_ENDPOINT");
  const char* region = std::getenv("SWORDFS_TEST_S3_REGION");
  const char* bucket = std::getenv("SWORDFS_TEST_S3_BUCKET");
  if (!endpoint || !region || !bucket) return false;
  cfg->endpoint = endpoint;
  cfg->region = region;
  cfg->bucket = bucket;
  const char* prefix = std::getenv("SWORDFS_TEST_S3_PREFIX");
  cfg->prefix = prefix ? prefix : "swordfs-test";
  return true;
}

// ── Chunk key convention (must match VfsImpl::ChunkKey) ─────────────────

// Chunk N covers file offsets [N*max_chunk_size, (N+1)*max_chunk_size).
// This format is a CONTRACT between VfsImpl and IDataEngine — both sides
// must agree for Read to locate chunks without metadata lookups.
std::string ChunkKey(uint64_t ino, size_t file_offset, size_t max_chunk_size) {
  uint64_t seq = file_offset / max_chunk_size;
  return "chunks/" + std::to_string(ino) + "/" + std::to_string(seq);
}

// ── Simulated VFS Write buffer ──────────────────────────────────────────

struct WriteBuffer {
  std::string data;
  uint64_t ino;
};

/// Flush one write buffer to S3, splitting into chunk-sized segments.
/// Returns the number of chunks uploaded, or -1 on error.
int FlushBuffer(IDataEngine* engine, const WriteBuffer& wb,
                size_t max_chunk_size) {
  int chunks = 0;
  for (size_t off = 0; off < wb.data.size(); off += max_chunk_size) {
    size_t seg_size = std::min(max_chunk_size, wb.data.size() - off);
    std::string key = ChunkKey(wb.ino, off, max_chunk_size);
    std::string_view seg(wb.data.data() + off, seg_size);

    Status s = engine->Put(key, seg);
    if (!s.ok()) return -1;
    ++chunks;
  }
  return chunks;
}

/// Read data back using the same chunk key convention that VfsImpl::Read
/// uses.  Returns the data, or empty string on error.
std::string ReadChunked(IDataEngine* engine, uint64_t ino, size_t offset,
                        size_t size, size_t max_chunk_size) {
  size_t chunk_seq = offset / max_chunk_size;
  size_t chunk_off = offset % max_chunk_size;
  std::string key = ChunkKey(ino, chunk_seq * max_chunk_size, max_chunk_size);

  std::string result;
  Status s = engine->Get(key, &result, chunk_off, size);
  if (!s.ok()) return "";
  return result;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────

class S3IntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    S3Config cfg;
    if (!LoadConfig(&cfg)) {
      GTEST_SKIP() << "S3 env vars not set: SWORDFS_TEST_S3_ENDPOINT, "
                      "SWORDFS_TEST_S3_REGION, SWORDFS_TEST_S3_BUCKET";
    }
    engine_ = std::make_unique<S3DataEngine>(cfg);
    max_chunk_size_ = engine_->Limits().max_chunk_size;
  }

  void TearDown() override {
    // Clean up all keys created during this test.
    for (const auto& key : keys_) {
      engine_->Delete(key);
    }
  }

  /// Track a key for cleanup on teardown.
  void TrackKey(std::string_view key) {
    keys_.emplace_back(key);
  }

  std::unique_ptr<S3DataEngine> engine_;
  std::vector<std::string> keys_;
  size_t max_chunk_size_ = 64 * 1024 * 1024;
};

// ────────────────────────────────────────────────────────────────────────
// Single-chunk write → flush → read (the basic data-path contract)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, WriteFlushReadSingleChunk) {
  constexpr uint64_t kIno = 100;
  const std::string data = "Hello SwordFS! This is an integration test.";

  // Flush the buffer to S3.
  WriteBuffer wb{data, kIno};
  int n = FlushBuffer(engine_.get(), wb, max_chunk_size_);
  ASSERT_GE(n, 1) << "FlushBuffer failed";

  // Track keys for cleanup.
  for (size_t off = 0; off < data.size(); off += max_chunk_size_) {
    TrackKey(ChunkKey(kIno, off, max_chunk_size_));
  }

  // Read back the full file.
  std::string result = ReadChunked(engine_.get(), kIno, 0, data.size(),
                                   max_chunk_size_);
  EXPECT_EQ(result, data);
}

// ────────────────────────────────────────────────────────────────────────
// Multi-chunk write → flush → read (validates chunk segmentation)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, WriteFlushReadMultiChunk) {
  constexpr uint64_t kIno = 200;
  // 3 chunks worth of data (but small enough to fit in test memory).
  constexpr size_t kChunkSize = 64 * 1024;  // 64 KiB for this test
  const size_t kTotalSize = kChunkSize * 3 + 100;  // 3+ partial chunks

  std::string data(kTotalSize, '\0');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>((i * 7 + 13) % 251 + 1);
  }

  WriteBuffer wb{data, kIno};
  int n = FlushBuffer(engine_.get(), wb, max_chunk_size_);
  ASSERT_GE(n, 1) << "FlushBuffer failed";

  for (size_t off = 0; off < data.size(); off += max_chunk_size_) {
    TrackKey(ChunkKey(kIno, off, max_chunk_size_));
  }

  // Read at various offsets and sizes.
  struct {
    size_t offset;
    size_t size;
  } reads[] = {
      {0, 100},               // start
      {kChunkSize - 50, 100}, // spanning chunk boundary
      {kChunkSize, 200},      // second chunk
      {kChunkSize * 2, 50},   // third chunk
      {data.size() - 10, 10},  // tail
      {0, data.size()},        // full file
  };

  for (const auto& r : reads) {
    std::string expected = data.substr(r.offset, r.size);
    std::string result = ReadChunked(engine_.get(), kIno, r.offset, r.size,
                                     max_chunk_size_);
    EXPECT_EQ(result, expected)
        << "Mismatch at offset=" << r.offset << " size=" << r.size;
  }
}

// ────────────────────────────────────────────────────────────────────────
// Chunk key determinism (contract verification)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, ChunkKeysAreDeterministic) {
  // Two calls with the same (ino, offset) must produce the same key.
  // This is the fundamental contract that lets Read locate chunks
  // without consulting metadata.
  std::string k1 = ChunkKey(42, 0, max_chunk_size_);
  std::string k2 = ChunkKey(42, 0, max_chunk_size_);
  EXPECT_EQ(k1, k2);

  // Different offsets produce different chunk seq numbers.
  std::string k3 = ChunkKey(42, max_chunk_size_, max_chunk_size_);
  EXPECT_NE(k1, k3);
  EXPECT_EQ(k3, "chunks/42/1");
}

TEST_F(S3IntegrationTest, ChunkKeyFormat) {
  // The key format is: chunks/{inode}/{seq_number}
  EXPECT_EQ(ChunkKey(0, 0, max_chunk_size_), "chunks/0/0");
  EXPECT_EQ(ChunkKey(1, 0, max_chunk_size_), "chunks/1/0");
  EXPECT_EQ(ChunkKey(1, max_chunk_size_, max_chunk_size_), "chunks/1/1");
  EXPECT_EQ(ChunkKey(1, max_chunk_size_ * 5, max_chunk_size_), "chunks/1/5");
}

// ────────────────────────────────────────────────────────────────────────
// Read at offset within a chunk (range request)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, ReadWithOffset) {
  constexpr uint64_t kIno = 300;
  const std::string data = "abcdefghijklmnopqrstuvwxyz";

  WriteBuffer wb{data, kIno};
  ASSERT_GE(FlushBuffer(engine_.get(), wb, max_chunk_size_), 1);
  TrackKey(ChunkKey(kIno, 0, max_chunk_size_));

  // Read from offset 10, 8 bytes → "klmnopqr"
  std::string result = ReadChunked(engine_.get(), kIno, 10, 8,
                                   max_chunk_size_);
  EXPECT_EQ(result, "klmnopqr");
}

TEST_F(S3IntegrationTest, ReadBeyondEndReturnsAvailable) {
  constexpr uint64_t kIno = 400;
  const std::string data = "short";

  WriteBuffer wb{data, kIno};
  ASSERT_GE(FlushBuffer(engine_.get(), wb, max_chunk_size_), 1);
  TrackKey(ChunkKey(kIno, 0, max_chunk_size_));

  // Request more bytes than available — the S3 range request returns
  // whatever is there.
  std::string result = ReadChunked(engine_.get(), kIno, 2, 100,
                                   max_chunk_size_);
  EXPECT_EQ(result, "ort");
}

// ────────────────────────────────────────────────────────────────────────
// Read non-existent inode → NotFound
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, ReadNonExistentChunk) {
  constexpr uint64_t kIno = 99999;
  std::string key = ChunkKey(kIno, 0, max_chunk_size_);

  // Ensure the key doesn't exist.
  engine_->Delete(key);

  std::string result;
  Status s = engine_->Get(key, &result);
  EXPECT_TRUE(s.IsNotFound())
      << "Expected NotFound for non-existent chunk, got: " << s.message();
}

// ────────────────────────────────────────────────────────────────────────
// Empty write
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, EmptyWriteNoChunks) {
  constexpr uint64_t kIno = 500;
  WriteBuffer wb{"", kIno};

  // Empty buffer should produce zero chunks.
  int n = FlushBuffer(engine_.get(), wb, max_chunk_size_);
  EXPECT_EQ(n, 0);

  // Reading from an empty file — key doesn't exist, should get NotFound.
  std::string result;
  Status s = engine_->Get(ChunkKey(kIno, 0, max_chunk_size_), &result);
  EXPECT_TRUE(s.IsNotFound());
}

// ────────────────────────────────────────────────────────────────────────
// Multiple files (independent inodes don't collide)
// ────────────────────────────────────────────────────────────────────────

TEST_F(S3IntegrationTest, MultipleFilesIndependent) {
  WriteBuffer file_a{"data for file A", 100};
  WriteBuffer file_b{"different data for file B", 200};

  ASSERT_GE(FlushBuffer(engine_.get(), file_a, max_chunk_size_), 1);
  ASSERT_GE(FlushBuffer(engine_.get(), file_b, max_chunk_size_), 1);

  TrackKey(ChunkKey(100, 0, max_chunk_size_));
  TrackKey(ChunkKey(200, 0, max_chunk_size_));

  std::string ra = ReadChunked(engine_.get(), 100, 0, file_a.data.size(),
                               max_chunk_size_);
  EXPECT_EQ(ra, file_a.data);

  std::string rb = ReadChunked(engine_.get(), 200, 0, file_b.data.size(),
                               max_chunk_size_);
  EXPECT_EQ(rb, file_b.data);

  // The two inodes use different key prefixes — they don't collide.
  EXPECT_NE(ChunkKey(100, 0, max_chunk_size_),
            ChunkKey(200, 0, max_chunk_size_));
}

}  // namespace

#else  // !SWORDFS_ENABLE_S3

#include <gtest/gtest.h>
TEST(S3IntegrationTest, DISABLED_S3NotEnabled) {
  GTEST_SKIP() << "S3 support not compiled (ENABLE_S3=OFF)";
}

#endif  // SWORDFS_ENABLE_S3
