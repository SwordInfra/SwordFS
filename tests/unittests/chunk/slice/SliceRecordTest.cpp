// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "chunk/slice/SliceRecord.hpp"

namespace swordfs::chunk::slice {
namespace {

using Records = std::vector<SliceRecord>;
using Spans = std::vector<ResolvedSlice>;

TEST(SliceRecordTest, SliceIdentityIsStronglyTypedAndOrderedButNotCommitOrder) {
  const SliceID zero;
  const SliceID first(1);
  const SliceID last(static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  EXPECT_EQ(zero.Value(), 0);
  EXPECT_EQ(first.Value(), 1);
  EXPECT_EQ(last.Value(), static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  EXPECT_EQ(zero, SliceID(0));
  EXPECT_NE(first, last);
  EXPECT_LT(first, last);
  EXPECT_GT(last, zero);
  EXPECT_EQ(first <=> SliceID(1), std::strong_ordering::equal);
}

TEST(SliceRecordTest, DefaultRecordsAreNotValidCommittedData) {
  const SliceRecord uninitialized_record;
  EXPECT_EQ(uninitialized_record.pos, 0);
  EXPECT_EQ(uninitialized_record.id, SliceID());
  EXPECT_EQ(uninitialized_record.size, 0);
  EXPECT_EQ(uninitialized_record.off, 0);
  EXPECT_EQ(uninitialized_record.len, 0);

  const ResolvedSlice empty_span;
  EXPECT_EQ(empty_span.pos, 0);
  EXPECT_EQ(empty_span.id, SliceID());
  EXPECT_EQ(empty_span.off, 0);
  EXPECT_EQ(empty_span.len, 0);

  Spans out;
  EXPECT_FALSE(ResolveRange(Records{uninitialized_record}, 64, 0, 1, &out).ok());
  EXPECT_TRUE(out.empty());
}

Spans Resolve(const Records &records, uint32_t size, uint32_t pos, uint32_t len) {
  Spans out;
  auto status = ResolveRange(records, size, pos, len, &out);
  EXPECT_TRUE(status.ok()) << status.message();
  return out;
}

void ExpectSpans(const Spans &actual, const Spans &expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i].pos, expected[i].pos) << i;
    EXPECT_EQ(actual[i].id, expected[i].id) << i;
    EXPECT_EQ(actual[i].off, expected[i].off) << i;
    EXPECT_EQ(actual[i].len, expected[i].len) << i;
  }
}

TEST(SliceRecordTest, EmptyHistoryAndEmptyRequest) {
  ExpectSpans(Resolve({}, 64, 8, 20), {{8, SliceID(), 0, 20}});
  ExpectSpans(Resolve({}, 64, 64, 0), {});
}

TEST(SliceRecordTest, OverlaysRespectAppendOrderNotIdAllocationOrder) {
  Records records = {{0, SliceID(50), 16, 0, 16}, {4, SliceID(60), 4, 0, 4}, {6, SliceID(1), 5, 0, 5}};
  ExpectSpans(Resolve(records, 64, 0, 16),
              {{0, SliceID(50), 0, 4}, {4, SliceID(60), 0, 2}, {6, SliceID(1), 0, 5}, {11, SliceID(50), 11, 5}});
  ExpectSpans(Resolve(records, 64, 3, 10),
              {{3, SliceID(50), 3, 1}, {4, SliceID(60), 0, 2}, {6, SliceID(1), 0, 5}, {11, SliceID(50), 11, 2}});
}

TEST(SliceRecordTest, ClippedObjectOffsetsAndZeroCover) {
  Records records = {{0, SliceID(10), 24, 4, 12}, {4, SliceID(), 0, 0, 4}, {6, SliceID(9), 2, 0, 2}};
  ExpectSpans(Resolve(records, 20, 0, 20), {{0, SliceID(10), 4, 4},
                                            {4, SliceID(), 0, 2},
                                            {6, SliceID(9), 0, 2},
                                            {8, SliceID(10), 12, 4},
                                            {12, SliceID(), 0, 8}});
}

TEST(SliceRecordTest, AdjacentPhysicalSpansMergeOnlyWhenContiguous) {
  Records records = {
      {0, SliceID(10), 16, 2, 4}, {4, SliceID(10), 16, 6, 4}, {8, SliceID(10), 16, 12, 2}, {10, SliceID(11), 2, 0, 2}};
  ExpectSpans(Resolve(records, 16, 0, 16),
              {{0, SliceID(10), 2, 8}, {8, SliceID(10), 12, 2}, {10, SliceID(11), 0, 2}, {12, SliceID(), 0, 4}});
}

TEST(SliceRecordTest, GrowingSliceMayAppearWithDifferentSizesInCommittedHistory) {
  // One Slice may extend its existing contiguous prefix; the two records
  // describe different points in its growth, not different data objects.
  const Records records = {
      {0, SliceID(7), 4, 0, 4},
      {4, SliceID(7), 8, 4, 4},
  };
  ExpectSpans(Resolve(records, 16, 0, 12), {
                                               {0, SliceID(7), 0, 8},
                                               {8, SliceID(), 0, 4},
                                           });

  // Read planning must remain correct when a later Slice partially masks the
  // grown Slice. Append/freeze decisions belong to the writer, not this API.
  const Records overlaid = {
      {0, SliceID(7), 4, 0, 4},
      {4, SliceID(7), 8, 4, 4},
      {2, SliceID(8), 2, 0, 2},
  };
  ExpectSpans(Resolve(overlaid, 16, 0, 8), {
                                               {0, SliceID(7), 0, 2},
                                               {2, SliceID(8), 0, 2},
                                               {4, SliceID(7), 4, 4},
                                           });
}

TEST(SliceRecordTest, IdenticalStartAndChunkBoundary) {
  Records records = {{0, SliceID(12), 64, 0, 64}, {0, SliceID(3), 4, 0, 4}, {60, SliceID(4), 4, 0, 4}};
  ExpectSpans(Resolve(records, 64, 0, 64), {{0, SliceID(3), 0, 4}, {4, SliceID(12), 4, 56}, {60, SliceID(4), 0, 4}});
}

TEST(SliceRecordTest, RejectsMalformedAndOutOfRangeRecordsAtomically) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  const std::vector<Records> invalid = {
      {{0, SliceID(1), 1, 0, 0}},       {{31, SliceID(1), 4, 0, 2}},
      {{0, SliceID(1), 0, 0, 1}},       {{0, SliceID(1), 4, 3, 2}},
      {{0, SliceID(1), 4, 5, 1}},       {{0, SliceID(), 1, 0, 1}},
      {{0, SliceID(), 0, 1, 1}},        {{0, SliceID(std::numeric_limits<uint64_t>::max()), 1, 0, 1}},
      {{max, SliceID(1), max, max, 1}},
  };
  for (const auto &records : invalid) {
    Spans out = {{1, SliceID(), 0, 3}};
    EXPECT_FALSE(ResolveRange(records, 32, 0, 8, &out).ok());
    ExpectSpans(out, {{1, SliceID(), 0, 3}});
  }
  const Records valid = {{0, SliceID(1), 32, 0, 32}};
  Spans out;
  EXPECT_FALSE(ResolveRange(valid, 32, 33, 0, &out).ok());
  EXPECT_FALSE(ResolveRange(valid, 32, 31, 2, &out).ok());
  EXPECT_FALSE(ResolveRange(valid, 0, 0, 0, &out).ok());
  EXPECT_FALSE(ResolveRange(valid, 32, 0, 1, nullptr).ok());
  EXPECT_TRUE(out.empty());
}

TEST(SliceRecordTest, ValidatesAllHistoryIncludingOutsideRequestedRangeAndEmptyReads) {
  // An error in an older, covered/out-of-range entry must not be accepted as
  // a harmless hole by a caller reading a different span.
  const Records invalid_history = {
      {8, SliceID(1), 3, 0, 4},
      {0, SliceID(2), 4, 0, 4},
  };
  Spans out = {{17, SliceID(), 0, 3}};
  EXPECT_FALSE(ResolveRange(invalid_history, 64, 0, 4, &out).ok());
  ExpectSpans(out, {{17, SliceID(), 0, 3}});
  EXPECT_FALSE(ResolveRange(invalid_history, 64, 64, 0, &out).ok());
  ExpectSpans(out, {{17, SliceID(), 0, 3}});

  const Records valid_history = {{0, SliceID(2), 4, 0, 4}};
  ASSERT_TRUE(ResolveRange(valid_history, 64, 64, 0, &out).ok());
  EXPECT_TRUE(out.empty());
}

TEST(SliceRecordTest, FullyCoveredOlderRecordsDoNotAlterVisibleRead) {
  const Records records = {
      {0, SliceID(11), 32, 0, 32},
      {0, SliceID(12), 32, 0, 32},
      {0, SliceID(), 0, 0, 32},
      {8, SliceID(13), 20, 4, 16},
  };
  ExpectSpans(Resolve(records, 32, 0, 32), {
                                               {0, SliceID(), 0, 8},
                                               {8, SliceID(13), 4, 16},
                                               {24, SliceID(), 0, 8},
                                           });
}

TEST(SliceRecordTest, MaximumSupportedChunkLocalRangeDoesNotOverflow) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  const Records records = {{0, SliceID(1), max, 0, max}};
  ExpectSpans(Resolve(records, max, max - 4, 4), {{max - 4, SliceID(1), max - 4, 4}});
}

TEST(SliceRecordTest, SeededRandomOverwritesMatchByteArrayOracle) {
  constexpr uint32_t kChunkSize = 128;
  for (uint32_t seed = 0; seed < 24; ++seed) {
    std::mt19937 rng(seed + 2026);
    Records records;
    std::unordered_map<uint64_t, std::string> objects;
    std::string reference(kChunkSize, '\0');
    for (uint32_t step = 0; step < 120; ++step) {
      const uint32_t pos = rng() % kChunkSize;
      const uint32_t len = 1 + rng() % std::min<uint32_t>(kChunkSize - pos, 24);
      const bool zero = rng() % 5 == 0;
      const SliceID id(zero ? 0 : 3000 - step);
      const uint32_t size = zero ? 0 : len + rng() % 8;
      const uint32_t off = zero ? 0 : rng() % (size - len + 1);
      std::string object(size, '\0');
      for (auto &c : object) {
        c = static_cast<char>('a' + rng() % 26);
      }
      if (!zero) {
        objects.emplace(id.Value(), object);
      }
      records.push_back({pos, id, size, off, len});
      reference.replace(pos, len, zero ? std::string(len, '\0') : object.substr(off, len));

      const uint32_t read_pos = rng() % kChunkSize;
      const uint32_t read_len = rng() % (kChunkSize - read_pos + 1);
      const auto spans = Resolve(records, kChunkSize, read_pos, read_len);
      uint32_t cursor = read_pos;
      std::string actual;
      for (const auto &span : spans) {
        EXPECT_EQ(span.pos, cursor) << seed << ":" << step;
        cursor += span.len;
        actual +=
            span.id == SliceID() ? std::string(span.len, '\0') : objects.at(span.id.Value()).substr(span.off, span.len);
      }
      EXPECT_EQ(cursor, read_pos + read_len) << seed << ":" << step;
      EXPECT_EQ(actual, reference.substr(read_pos, read_len)) << seed << ":" << step;
    }
  }
}

}  // namespace
}  // namespace swordfs::chunk::slice
