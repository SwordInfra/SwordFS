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

static auto Buf(const std::string& s) {
  return *folly::IOBuf::copyBuffer(s.data(), s.size());
}

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
  wb.Write(0, Buf(Repeat('X', 100)));
  EXPECT_EQ(wb.size(), 100);

  wb = WriteBuf(512);
  EXPECT_EQ(wb.size(), 0);
}

// ================================================================
// Write
// ================================================================

TEST(WriteBufTest, WriteAccumulatesData) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf("hello"));
  EXPECT_EQ(wb.size(), 5);
  wb.Write(5, Buf("world"));
  EXPECT_EQ(wb.size(), 10);
}

TEST(WriteBufTest, WriteExtendsBuffer) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 100)));
  EXPECT_EQ(wb.size(), 100);
  wb.Write(100, Buf(Repeat('B', 50)));
  EXPECT_EQ(wb.size(), 150);
  wb.Write(0, Buf(Repeat('C', 30)));
  EXPECT_EQ(wb.size(), 150);
}

TEST(WriteBufTest, WritePastCapacity) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('X', kChunkSize)));
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
  wb.Write(0, Buf(Repeat('X', kChunkSize - 1)));
  EXPECT_EQ(wb.size(), kChunkSize - 1);
}

TEST(WriteBufTest, SizeAtThreshold) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('X', kChunkSize)));
  EXPECT_EQ(wb.size(), kChunkSize);
}

// ================================================================
// FlushData
// ================================================================

TEST(WriteBufTest, FlushDataReturnsAllBelowThreshold) {
  WriteBuf wb(kChunkSize);
  std::string payload = Repeat('Y', kChunkSize / 2);
  wb.Write(0, Buf(payload));

  std::string_view data = wb.FlushData();
  EXPECT_EQ(data.size(), payload.size());
  EXPECT_EQ(data, payload);
}

TEST(WriteBufTest, FlushDataReturnsFullChunk) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('Z', kChunkSize)));

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
  wb.Write(0, Buf(Repeat('A', 200)));

  auto out = folly::IOBuf::create(256);
  ASSERT_TRUE(wb.CopyOut(0, 200, out.get()).ok());
  EXPECT_EQ(out->length(), 200);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', 200));
}

TEST(WriteBufTest, CopyOutWithOffset) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 200)));

  auto out = folly::IOBuf::create(256);
  ASSERT_TRUE(wb.CopyOut(100, 50, out.get()).ok());
  EXPECT_EQ(out->length(), 50);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', 50));
}

TEST(WriteBufTest, CopyOutTruncatesAtEnd) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 100)));

  auto out = folly::IOBuf::create(256);
  ASSERT_TRUE(wb.CopyOut(50, 200, out.get()).ok());
  EXPECT_EQ(out->length(), 50);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()),
                             out->length()),
            Repeat('A', 50));
}

TEST(WriteBufTest, WriteOverwriteWithinBounds) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 200)));
  wb.Write(50, Buf(Repeat('B', 50)));

  EXPECT_EQ(wb.size(), 200);
  auto out = folly::IOBuf::create(256);
  ASSERT_TRUE(wb.CopyOut(0, 200, out.get()).ok());
  std::string_view sv(reinterpret_cast<const char *>(out->data()),
                      out->length());
  EXPECT_EQ(sv.substr(0, 50), Repeat('A', 50));
  EXPECT_EQ(sv.substr(50, 50), Repeat('B', 50));
  EXPECT_EQ(sv.substr(100, 100), Repeat('A', 100));
}

TEST(WriteBufTest, WriteExtendsBufferNonContiguous) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 100)));
  wb.Write(150, Buf(Repeat('B', 50)));
  EXPECT_EQ(wb.size(), 200);
}

TEST(WriteBufTest, CopyOutNegativeOffset) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 100)));

  auto out = folly::IOBuf::create(256);
  EXPECT_FALSE(wb.CopyOut(-1, 50, out.get()).ok());
}

TEST(WriteBufTest, CopyOutBufferTooSmall) {
  WriteBuf wb(kChunkSize);
  wb.Write(0, Buf(Repeat('A', 100)));

  auto out = folly::IOBuf::create(10);
  EXPECT_FALSE(wb.CopyOut(0, 50, out.get()).ok());
}

// ────────────────────────────────────────────────────────────────
// Positional writes — each chunk gets a window IOBuf pointing to
// its correct position in the parent buffer.  Even with out-of-order
// arrival, data lands at the right file offset.
// ────────────────────────────────────────────────────────────────

TEST(WriteBufTest, CopyOutPositionalWrite) {
  WriteBuf chunk0(kChunkSize);
  WriteBuf chunk1(kChunkSize);
  chunk0.Write(0, Buf(Repeat('A', 200)));
  chunk1.Write(0, Buf(Repeat('B', 200)));

  auto out = folly::IOBuf::create(400);

  // chunk1 at file offset 200 → window into out + 200.
  auto w1 = folly::IOBuf::takeOwnership(
      const_cast<uint8_t *>(out->writableData()) + 200, 200, (std::size_t)0,
      +[](void *, void *) {}, nullptr, true);
  ASSERT_TRUE(chunk1.CopyOut(0, 200, w1.get()).ok());

  // chunk0 at file offset 0 → window into out + 0.
  auto w0 = folly::IOBuf::takeOwnership(
      out->writableData(), 200, (std::size_t)0,
      +[](void *, void *) {}, nullptr, true);
  ASSERT_TRUE(chunk0.CopyOut(0, 200, w0.get()).ok());

  out->append(400);
  std::string_view sv(reinterpret_cast<const char *>(out->data()),
                      out->length());
  EXPECT_EQ(sv.substr(0, 200), Repeat('A', 200));
  EXPECT_EQ(sv.substr(200, 200), Repeat('B', 200));
}

TEST(WriteBufTest, CopyOutPositionalWithGap) {
  static constexpr size_t kHalf = kChunkSize / 2;
  WriteBuf chunk0(kChunkSize);
  WriteBuf chunk2(kChunkSize);
  auto a = Repeat('A', kHalf);
  auto c = Repeat('C', kHalf);
  chunk0.Write(0, Buf(a));
  chunk2.Write(0, Buf(c));

  auto out = folly::IOBuf::create(kChunkSize);

  // chunk2 at file offset kHalf.
  auto w2 = folly::IOBuf::takeOwnership(
      const_cast<uint8_t *>(out->writableData()) + kHalf, kHalf, (std::size_t)0,
      +[](void *, void *) {}, nullptr, true);
  ASSERT_TRUE(chunk2.CopyOut(0, kHalf, w2.get()).ok());

  // chunk0 at file offset 0.
  auto w0 = folly::IOBuf::takeOwnership(
      out->writableData(), kHalf, (std::size_t)0,
      +[](void *, void *) {}, nullptr, true);
  ASSERT_TRUE(chunk0.CopyOut(0, kHalf, w0.get()).ok());

  out->append(kChunkSize);
  std::string_view sv(reinterpret_cast<const char *>(out->data()),
                      out->length());
  EXPECT_EQ(sv.substr(0, kHalf), a);
  EXPECT_EQ(sv.substr(kHalf, kHalf), c);
}

// ================================================================
// Edge cases
// ================================================================

TEST(WriteBufTest, EmptyBufferCopyOut) {
  WriteBuf wb(kChunkSize);
  auto out = folly::IOBuf::create(256);
  ASSERT_TRUE(wb.CopyOut(0, 100, out.get()).ok());
  EXPECT_EQ(out->length(), 0);
}

TEST(WriteBufTest, ZeroSizeWrite) {
  WriteBuf wb(kChunkSize);
  auto empty = folly::IOBuf::create(0);
  wb.Write(0, *empty);
  EXPECT_EQ(wb.size(), 0);
}

TEST(WriteBufTest, EmptyBufferFlushData) {
  WriteBuf wb(kChunkSize);
  EXPECT_TRUE(wb.FlushData().empty());
}

TEST(WriteBufTest, WritePastCapacityReturnsError) {
  WriteBuf wb(kChunkSize);
  Status st = wb.Write(kChunkSize, Buf(Repeat('X', 10)));
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(wb.size(), 0);  // nothing written
}

TEST(WriteBufTest, WriteNegativeOffsetReturnsError) {
  WriteBuf wb(kChunkSize);
  Status st = wb.Write(-1, Buf(Repeat('X', 10)));
  EXPECT_FALSE(st.ok());
}
