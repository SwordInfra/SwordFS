// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <algorithm>

#include "storage/Slice.hpp"

using swordfs::storage::ChunkMetaKey;
using swordfs::storage::Slice;
using swordfs::storage::SliceList;

// ════════════════════════════════════════════════════════════════════
// SliceTest
// ════════════════════════════════════════════════════════════════════

TEST(SliceTest, DefaultConstructed) {
  Slice s;
  EXPECT_EQ(s.id, 0);
  EXPECT_EQ(s.offset, 0);
  EXPECT_EQ(s.length, 0);
}

TEST(SliceTest, SortByOffset) {
  Slice a{1, 100, 50};
  Slice b{2, 0, 50};
  Slice c{3, 200, 50};
  std::vector<Slice> slices = {a, b, c};
  std::sort(slices.begin(), slices.end());
  EXPECT_EQ(slices[0].offset, 0);
  EXPECT_EQ(slices[1].offset, 100);
  EXPECT_EQ(slices[2].offset, 200);
}

TEST(SliceTest, AllocID) {
  SliceList list;
  EXPECT_EQ(list.AllocID(), 1);
  EXPECT_EQ(list.AllocID(), 2);
  EXPECT_EQ(list.AllocID(), 3);
  EXPECT_EQ(list.next_slice_id, 4);
}

TEST(SliceTest, SliceListEmpty) {
  SliceList list;
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0);
}

TEST(SliceTest, SerializeRoundtrip) {
  SliceList list;
  list.AllocID();  // advance to 1
  list.slices.push_back({1, 0, 100});
  list.slices.push_back({2, 100, 50});
  list.slices.push_back({3, 200, 75});

  std::string data = list.Serialize();
  EXPECT_GT(data.size(), 0);

  SliceList restored;
  EXPECT_TRUE(restored.Deserialize(data));
  EXPECT_EQ(restored.next_slice_id, list.next_slice_id);
  EXPECT_EQ(restored.size(), 3);
  EXPECT_EQ(restored.slices[0].id, 1);
  EXPECT_EQ(restored.slices[0].offset, 0);
  EXPECT_EQ(restored.slices[0].length, 100);
  EXPECT_EQ(restored.slices[1].id, 2);
  EXPECT_EQ(restored.slices[2].id, 3);
}

TEST(SliceTest, SerializeEmpty) {
  SliceList list;
  std::string data = list.Serialize();
  EXPECT_GT(data.size(), 0);  // still has next_slice_id + count

  SliceList restored;
  EXPECT_TRUE(restored.Deserialize(data));
  EXPECT_EQ(restored.next_slice_id, 1);
  EXPECT_TRUE(restored.empty());
}

TEST(SliceTest, DeserializeInvalidData) {
  SliceList list;
  EXPECT_FALSE(list.Deserialize("too_short"));
  EXPECT_FALSE(list.Deserialize(std::string_view()));
}

TEST(SliceTest, ChunkMetaKeyFormat) {
  EXPECT_EQ(ChunkMetaKey(1, 0), "A1C0");
  EXPECT_EQ(ChunkMetaKey(42, 5), "A42C5");
  EXPECT_EQ(ChunkMetaKey(0xFFFFFFFFFFFFFFFFULL, 0), "A18446744073709551615C0");
}
