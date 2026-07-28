// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for WriteBuf — memory-only, no mocks required.

#include <gtest/gtest.h>

#include <string>

#include "chunk/WriteBuf.hpp"
#include "utils/Status.hpp"

using swordfs::chunk::WriteBuf;
using swordfs::utils::Status;

static std::string Repeat(char c, size_t n) { return std::string(n, c); }

static constexpr size_t kChunkSize = 256;

// ================================================================
// Construction
// ================================================================

TEST(WriteBufTest, ConstructorSetsState) {
  WriteBuf wb(kChunkSize);
  EXPECT_EQ(wb.size(), 0);
}

TEST(WriteBufTest, ReassignClearsBuffer) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('X', 100).data(), 100, 0);
  EXPECT_EQ(wb.size(), 100);

  wb = WriteBuf(512);
  EXPECT_EQ(wb.size(), 0);
}

// ================================================================
// Write
// ================================================================

TEST(WriteBufTest, WriteAccumulatesData) {
  WriteBuf wb(kChunkSize);
  wb.Write("hello", 5, 0);
  EXPECT_EQ(wb.size(), 5);
  wb.Write("world", 5, 5);
  EXPECT_EQ(wb.size(), 10);
}

TEST(WriteBufTest, WriteExtendsBuffer) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 100).data(), 100, 0);
  EXPECT_EQ(wb.size(), 100);
  wb.Write(Repeat('B', 50).data(), 50, 100);
  EXPECT_EQ(wb.size(), 150);
  wb.Write(Repeat('C', 30).data(), 30, 0);
  EXPECT_EQ(wb.size(), 150);
}

TEST(WriteBufTest, WritePastCapacity) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('X', kChunkSize).data(), kChunkSize, 0);
  EXPECT_EQ(wb.size(), kChunkSize);
}

// ================================================================
// size()
// ================================================================

TEST(WriteBufTest, SizeZeroWhenEmpty) {
  WriteBuf wb(kChunkSize);
  EXPECT_EQ(wb.size(), 0);
}

TEST(WriteBufTest, SizeBelowThreshold) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('X', kChunkSize - 1).data(), kChunkSize - 1, 0);
  EXPECT_EQ(wb.size(), kChunkSize - 1);
}

TEST(WriteBufTest, SizeAtThreshold) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('X', kChunkSize).data(), kChunkSize, 0);
  EXPECT_EQ(wb.size(), kChunkSize);
}

// ================================================================
// FlushData
// ================================================================

TEST(WriteBufTest, FlushDataReturnsAllBelowThreshold) {
  WriteBuf wb(kChunkSize);
  std::string payload = Repeat('Y', kChunkSize / 2);
  wb.Write(payload.data(), payload.size(), 0);

  std::string_view data = wb.FlushData();
  EXPECT_EQ(data.size(), payload.size());
  EXPECT_EQ(data, payload);
}

TEST(WriteBufTest, FlushDataReturnsFullChunk) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('Z', kChunkSize).data(), kChunkSize, 0);

  std::string_view data = wb.FlushData();
  EXPECT_EQ(data.size(), kChunkSize);
  EXPECT_EQ(data, Repeat('Z', kChunkSize));
}

TEST(WriteBufTest, FlushDataEmpty) {
  WriteBuf wb(kChunkSize);
  EXPECT_TRUE(wb.FlushData().empty());
}

// ================================================================
// CopyOut
// ================================================================

TEST(WriteBufTest, CopyOutFullBuffer) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 200).data(), 200, 0);

  auto out = folly::IOBuf::create(256);
  Status st = wb.CopyOut(0, 200, out.get());
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(out->length(), 200);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out->data()),
                             out->length()),
            Repeat('A', 200));
}

TEST(WriteBufTest, CopyOutWithOffset) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 200).data(), 200, 0);

  auto out = folly::IOBuf::create(256);
  Status st = wb.CopyOut(100, 50, out.get());
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(out->length(), 50);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out->data()),
                             out->length()),
            Repeat('A', 50));
}

TEST(WriteBufTest, CopyOutTruncatesAtEnd) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 100).data(), 100, 0);

  auto out = folly::IOBuf::create(256);
  Status st = wb.CopyOut(50, 200, out.get());
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(out->length(), 50);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out->data()),
                             out->length()),
            Repeat('A', 50));
}

TEST(WriteBufTest, WriteOverwriteWithinBounds) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 200).data(), 200, 0);
  wb.Write(Repeat('B', 50).data(), 50, 50);

  EXPECT_EQ(wb.size(), 200);
  auto out = folly::IOBuf::create(256);
  Status st = wb.CopyOut(0, 200, out.get());
  EXPECT_TRUE(st.ok());
  std::string_view sv(reinterpret_cast<const char*>(out->data()),
                      out->length());
  EXPECT_EQ(sv.substr(0, 50), Repeat('A', 50));
  EXPECT_EQ(sv.substr(50, 50), Repeat('B', 50));
  EXPECT_EQ(sv.substr(100, 100), Repeat('A', 100));
}

TEST(WriteBufTest, WriteExtendsBufferNonContiguous) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 100).data(), 100, 0);
  wb.Write(Repeat('B', 50).data(), 50, 150);
  EXPECT_EQ(wb.size(), 200);
}

TEST(WriteBufTest, CopyOutNegativeOffset) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 100).data(), 100, 0);

  auto out = folly::IOBuf::create(256);
  Status st = wb.CopyOut(-1, 50, out.get());
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(out->length(), 0);
}

TEST(WriteBufTest, CopyOutBufferTooSmall) {
  WriteBuf wb(kChunkSize);
  wb.Write(Repeat('A', 100).data(), 100, 0);

  auto out = folly::IOBuf::create(10);  // only 10 bytes tailroom
  Status st = wb.CopyOut(0, 50, out.get());
  EXPECT_FALSE(st.ok());
}

// ================================================================
// Edge cases
// ================================================================

TEST(WriteBufTest, EmptyBufferCopyOut) {
  WriteBuf wb(kChunkSize);
  auto out = folly::IOBuf::create(256);
  Status st = wb.CopyOut(0, 100, out.get());
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(out->length(), 0);
}

TEST(WriteBufTest, ZeroSizeWrite) {
  WriteBuf wb(kChunkSize);
  wb.Write(nullptr, 0, 0);
  EXPECT_EQ(wb.size(), 0);
}

TEST(WriteBufTest, EmptyBufferFlushData) {
  WriteBuf wb(kChunkSize);
  EXPECT_TRUE(wb.FlushData().empty());
}

TEST(WriteBufTest, WritePastCapacityReturnsError) {
  WriteBuf wb(kChunkSize);
  Status st = wb.Write(Repeat('X', 10).data(), 10, kChunkSize);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(wb.size(), 0);  // nothing written
}

TEST(WriteBufTest, WriteNegativeOffsetReturnsError) {
  WriteBuf wb(kChunkSize);
  Status st = wb.Write(Repeat('X', 10).data(), 10, -1);
  EXPECT_FALSE(st.ok());
}
