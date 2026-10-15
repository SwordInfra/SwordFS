// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

#include "utils/Status.hpp"

namespace swordfs::chunk::slice {

// Identity of one contiguous logical Slice, which may grow by appending before
// it is frozen. The zero identity means a zero-cover, not a data object.
// SliceID is independent of ChunkID and COW.
class SliceID {
 public:
  constexpr SliceID() = default;
  explicit constexpr SliceID(uint64_t value) : value_(value) {
  }

  auto operator<=>(const SliceID &) const = default;

  constexpr uint64_t Value() const {
    return value_;
  }

 private:
  uint64_t value_ = 0;
};

// One entry in the ordered history belonging to a single logical ChunkID.
// Later entries overlay earlier entries. size is the Slice's length at the
// time of this record: multiple records with the same ID may have different
// sizes as a Slice grows. off/len can reference a clipped subrange.
struct SliceRecord {
  uint32_t pos = 0;
  SliceID id;
  uint32_t size = 0;
  uint32_t off = 0;
  uint32_t len = 0;
};

// Derived (not persisted) nonoverlapping read span. id=0 means emit zeroes.
struct ResolvedSlice {
  uint32_t pos = 0;
  SliceID id;
  uint32_t off = 0;
  uint32_t len = 0;
};

// Materialize the visible spans for [pos, pos+len), including zero holes.
// Validates the *entire* source history, even records outside the requested
// range; on error leaves the caller's output untouched.
utils::Status ResolveRange(std::span<const SliceRecord> records, uint32_t chunk_size, uint32_t pos, uint32_t len,
                           std::vector<ResolvedSlice> *out);

}  // namespace swordfs::chunk::slice
