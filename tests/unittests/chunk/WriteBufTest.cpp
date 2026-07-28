// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for WriteBuf — memory-only, no mocks required.

#include <gtest/gtest.h>

#include <string>

#include "chunk/WriteBuf.hpp"

using swordfs::chunk::WriteBuf;
using swordfs::metadata::InodeID;

// Helper
static std::string Repeat(char c, size_t n) {
  return std::string(n, c);
}

static constexpr InodeID kIno = 42;
static constexpr size_t kChunkSize = 256;

// ================================================================
// Init / IsInit
// ================================================================

TEST(WriteBufTest, DefaultNotInit) {
  WriteBuf wb;
  EXPECT_FALSE(wb.IsInit());
  EXPECT_EQ(wb.ino(), 0);
  EXPECT_TRUE(wb.empty());
}

TEST(WriteBufTest, InitSetsState) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  EXPECT_TRUE(wb.IsInit());
  EXPECT_EQ(wb.ino(), kIno);
  EXPECT_EQ(wb.max_chunk_size(), kChunkSize);
  EXPECT_TRUE(wb.empty());
  EXPECT_EQ(wb.next_chunk(), 0);
  EXPECT_EQ(wb.max_write_end(), 0);
}

TEST(WriteBufTest, ReInitClearsBuffer) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', 100).data(), 100, 0);
  EXPECT_EQ(wb.size(), 100);

  wb.Init(99, 512);
  EXPECT_EQ(wb.ino(), 99);
  EXPECT_EQ(wb.max_chunk_size(), 512);
  EXPECT_TRUE(wb.empty());
  EXPECT_EQ(wb.next_chunk(), 0);
  EXPECT_EQ(wb.max_write_end(), 0);
}

// ================================================================
// Append
// ================================================================

TEST(WriteBufTest, AppendAccumulatesData) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append("hello", 5, 0);
  EXPECT_EQ(wb.size(), 5);
  wb.Append("world", 5, 5);
  EXPECT_EQ(wb.size(), 10);
}

TEST(WriteBufTest, AppendTracksMaxWriteEnd) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  // Sequential writes
  wb.Append(Repeat('A', 100).data(), 100, 0);  // end = 100
  EXPECT_EQ(wb.max_write_end(), 100);
  wb.Append(Repeat('B', 50).data(), 50, 100);  // end = 150
  EXPECT_EQ(wb.max_write_end(), 150);

  // Non-contiguous write — max_write_end should not go backward
  wb.Append(Repeat('C', 30).data(), 30, 0);  // end = 30, but max stays 150
  EXPECT_EQ(wb.max_write_end(), 150);
}

TEST(WriteBufTest, AppendPastThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize + 10).data(), kChunkSize + 10, 0);
  EXPECT_EQ(wb.size(), kChunkSize + 10);
  EXPECT_TRUE(wb.ShouldFlush());
}

// ================================================================
// ShouldFlush
// ================================================================

TEST(WriteBufTest, ShouldFlushFalseWhenEmpty) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  EXPECT_FALSE(wb.ShouldFlush());
}

TEST(WriteBufTest, ShouldFlushFalseWhenBelowThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize - 1).data(), kChunkSize - 1, 0);
  EXPECT_FALSE(wb.ShouldFlush());
}

TEST(WriteBufTest, ShouldFlushTrueWhenAtThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize).data(), kChunkSize, 0);
  EXPECT_TRUE(wb.ShouldFlush());
}

TEST(WriteBufTest, ShouldFlushTrueWhenAboveThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize * 2).data(), kChunkSize * 2, 0);
  EXPECT_TRUE(wb.ShouldFlush());
}

// ================================================================
// FlushData (non-force)
// ================================================================

TEST(WriteBufTest, FlushDataNonForceReturnsExactChunkSize) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('A', kChunkSize).data(), kChunkSize, 0);

  std::string_view data = wb.FlushData(/*force=*/false);
  EXPECT_EQ(data.size(), kChunkSize);
  EXPECT_EQ(data, Repeat('A', kChunkSize));
}

TEST(WriteBufTest, FlushDataNonForceReturnsOnlyOneChunkWhenAboveThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  // Write 2.5 chunks: "AAA...BBB...CCC..."
  std::string all =
      Repeat('A', kChunkSize) + Repeat('B', kChunkSize) + Repeat('C', kChunkSize / 2);
  wb.Append(all.data(), all.size(), 0);

  std::string_view data = wb.FlushData(/*force=*/false);
  EXPECT_EQ(data.size(), kChunkSize);
  EXPECT_EQ(data, Repeat('A', kChunkSize));
}

TEST(WriteBufTest, FlushDataNonForceReturnsActualSizeWhenBelowThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize / 2).data(), kChunkSize / 2, 0);

  // FlushData(false) returns min(data_.size(), max_chunk_size_).
  // Caller should still check ShouldFlush() before committing.
  EXPECT_FALSE(wb.ShouldFlush());
  std::string_view data = wb.FlushData(/*force=*/false);
  EXPECT_EQ(data.size(), kChunkSize / 2);
  EXPECT_EQ(data, Repeat('X', kChunkSize / 2));
}

// ================================================================
// FlushData (force)
// ================================================================

TEST(WriteBufTest, FlushDataForceReturnsAllWhenBelowThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  std::string payload = Repeat('Y', kChunkSize / 2);
  wb.Append(payload.data(), payload.size(), 0);

  std::string_view data = wb.FlushData(/*force=*/true);
  EXPECT_EQ(data.size(), payload.size());
  EXPECT_EQ(data, payload);
}

TEST(WriteBufTest, FlushDataForceReturnsAllWhenAboveThreshold) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  size_t total = kChunkSize * 2 + 10;
  std::string payload = Repeat('Z', total);
  wb.Append(payload.data(), total, 0);

  std::string_view data = wb.FlushData(/*force=*/true);
  EXPECT_EQ(data.size(), total);
  EXPECT_EQ(data, payload);
}

TEST(WriteBufTest, FlushDataForceEmpty) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  std::string_view data = wb.FlushData(/*force=*/true);
  EXPECT_TRUE(data.empty());
}

// ================================================================
// CommitFlush
// ================================================================

TEST(WriteBufTest, CommitFlushRemovesFlushedBytes) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('A', kChunkSize + 50).data(), kChunkSize + 50, 0);

  EXPECT_TRUE(wb.ShouldFlush());
  size_t flushed = wb.FlushData(/*force=*/false).size();
  EXPECT_EQ(flushed, kChunkSize);
  wb.CommitFlush(flushed);

  // Remaining 50 bytes, next_chunk advanced.
  EXPECT_EQ(wb.size(), 50);
  EXPECT_EQ(wb.next_chunk(), 1);
  EXPECT_FALSE(wb.ShouldFlush());
}

TEST(WriteBufTest, CommitFlushAdvancesNextChunk) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize * 3).data(), kChunkSize * 3, 0);

  for (uint32_t i = 0; i < 3; i++) {
    EXPECT_EQ(wb.next_chunk(), i);
    size_t sz = wb.FlushData(/*force=*/false).size();
    EXPECT_EQ(sz, kChunkSize);
    wb.CommitFlush(sz);
  }
  EXPECT_EQ(wb.next_chunk(), 3);
  EXPECT_TRUE(wb.empty());
}

TEST(WriteBufTest, CommitFlushForceThenNonForce) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  // 2 full chunks + 10 bytes remainder.
  size_t total = kChunkSize * 2 + 10;
  wb.Append(Repeat('X', total).data(), total, 0);

  // Non-force flush chunks 0 and 1.
  for (int i = 0; i < 2; i++) {
    size_t sz = wb.FlushData(/*force=*/false).size();
    wb.CommitFlush(sz);
  }
  EXPECT_EQ(wb.next_chunk(), 2);
  EXPECT_EQ(wb.size(), 10);

  // Force-flush the remainder.
  size_t sz = wb.FlushData(/*force=*/true).size();
  EXPECT_EQ(sz, 10);
  wb.CommitFlush(sz);
  EXPECT_EQ(wb.next_chunk(), 3);
  EXPECT_TRUE(wb.empty());
}

// ================================================================
// BufStart / BufEnd
// ================================================================

TEST(WriteBufTest, BufStartIsZeroInitially) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  EXPECT_EQ(wb.BufStart(), 0);
  EXPECT_EQ(wb.BufEnd(), 0);
}

TEST(WriteBufTest, BufStartEndAfterAppend) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', 100).data(), 100, 0);

  EXPECT_EQ(wb.BufStart(), 0);
  EXPECT_EQ(wb.BufEnd(), 100);
}

TEST(WriteBufTest, BufStartAdvancesAfterCommitFlush) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', kChunkSize * 2).data(), kChunkSize * 2, 0);

  size_t sz = wb.FlushData(/*force=*/false).size();
  wb.CommitFlush(sz);

  // After flushing chunk 0, buffer data starts at chunk 1 offset.
  EXPECT_EQ(wb.BufStart(), static_cast<off_t>(kChunkSize));
  EXPECT_EQ(wb.BufEnd(), static_cast<off_t>(kChunkSize) + static_cast<off_t>(kChunkSize));
}

// ================================================================
// CopyOut
// ================================================================

TEST(WriteBufTest, CopyOutFullBuffer) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('A', 200).data(), 200, 0);

  std::string out;
  size_t n = wb.CopyOut(wb.BufStart(), 200, &out);
  EXPECT_EQ(n, 200);
  EXPECT_EQ(out, Repeat('A', 200));
}

TEST(WriteBufTest, CopyOutWithOffset) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('A', 200).data(), 200, 0);

  std::string out;
  size_t n = wb.CopyOut(wb.BufStart() + 100, 50, &out);
  EXPECT_EQ(n, 50);
  EXPECT_EQ(out, Repeat('A', 50));
}

TEST(WriteBufTest, CopyOutTruncatesAtEnd) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('A', 100).data(), 100, 0);

  std::string out;
  // Request more bytes than available.
  size_t n = wb.CopyOut(wb.BufStart() + 50, 200, &out);
  EXPECT_EQ(n, 50);
  EXPECT_EQ(out, Repeat('A', 50));
}

TEST(WriteBufTest, CopyOutAfterCommitFlush) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('A', kChunkSize).data(), kChunkSize, 0);

  // Flush chunk 0, then write chunk 1.
  size_t sz = wb.FlushData(/*force=*/false).size();
  wb.CommitFlush(sz);
  wb.Append(Repeat('B', 100).data(), 100, kChunkSize);

  // CopyOut at chunk 1 offset.
  std::string out;
  off_t buf_start = wb.BufStart();
  EXPECT_EQ(buf_start, static_cast<off_t>(kChunkSize));
  size_t n = wb.CopyOut(buf_start, 100, &out);
  EXPECT_EQ(n, 100);
  EXPECT_EQ(out, Repeat('B', 100));
}

// ================================================================
// Reset
// ================================================================

TEST(WriteBufTest, ResetClearsAllState) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(Repeat('X', 500).data(), 500, 0);
  size_t sz = wb.FlushData(/*force=*/false).size();
  wb.CommitFlush(sz);

  wb.Reset();

  EXPECT_TRUE(wb.empty());
  EXPECT_EQ(wb.next_chunk(), 0);
  EXPECT_EQ(wb.max_write_end(), 0);
  // ino and max_chunk_size are preserved (Reset doesn't change init state)
}

// ================================================================
// Empty buffer edge cases
// ================================================================

TEST(WriteBufTest, EmptyBufferFlushData) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  EXPECT_TRUE(wb.FlushData(false).empty());
  EXPECT_TRUE(wb.FlushData(true).empty());
}

TEST(WriteBufTest, EmptyBufferCopyOut) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  std::string out;
  size_t n = wb.CopyOut(0, 100, &out);
  EXPECT_EQ(n, 0);
  EXPECT_TRUE(out.empty());
}

TEST(WriteBufTest, ZeroSizeAppend) {
  WriteBuf wb;
  wb.Init(kIno, kChunkSize);
  wb.Append(nullptr, 0, 0);
  EXPECT_TRUE(wb.empty());
  EXPECT_EQ(wb.max_write_end(), 0);
}
