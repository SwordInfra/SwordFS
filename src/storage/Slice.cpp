// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/Slice.hpp"

#include <cstring>

namespace swordfs::storage {

// Wire format (little-endian):
//   [slice_count:4][next_slice_id:8][slice...]
//   each slice: [id:8][offset:4][length:4]

static constexpr size_t kHeaderSize = 12;  // 4 (count) + 8 (next_id)
static constexpr size_t kSliceSize = 16;   // 8 + 4 + 4

static void WriteU32(std::string& out, uint32_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

static void WriteU64(std::string& out, uint64_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

static bool ReadU32(const char*& p, const char* end, uint32_t* out) {
  if (p + sizeof(uint32_t) > end) return false;
  std::memcpy(out, p, sizeof(uint32_t));
  p += sizeof(uint32_t);
  return true;
}

static bool ReadU64(const char*& p, const char* end, uint64_t* out) {
  if (p + sizeof(uint64_t) > end) return false;
  std::memcpy(out, p, sizeof(uint64_t));
  p += sizeof(uint64_t);
  return true;
}

std::string SliceList::Serialize() const {
  std::string out;
  out.reserve(kHeaderSize + slices.size() * kSliceSize);

  WriteU32(out, static_cast<uint32_t>(slices.size()));
  WriteU64(out, next_slice_id);
  for (const auto& s : slices) {
    WriteU64(out, s.id);
    WriteU32(out, s.offset);
    WriteU32(out, s.length);
  }
  return out;
}

bool SliceList::Deserialize(std::string_view data) {
  const char* p = data.data();
  const char* end = p + data.size();

  uint32_t count = 0;
  if (!ReadU32(p, end, &count)) return false;
  if (!ReadU64(p, end, &next_slice_id)) return false;

  slices.clear();
  slices.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Slice s;
    if (!ReadU64(p, end, &s.id)) return false;
    if (!ReadU32(p, end, &s.offset)) return false;
    if (!ReadU32(p, end, &s.length)) return false;
    slices.push_back(s);
  }

  return true;
}

}  // namespace swordfs::storage
