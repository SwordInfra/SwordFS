// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

#include "utils/Status.hpp"

namespace swordfs::chunk::slice {

// Identity of one immutable physical Slice object. Multiple disjoint logical
// records may reference different physical offsets of this same object;
// the zero identity denotes a zero-cover rather than stored data.
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
// Later entries overlay earlier entries. size is the immutable physical
// object's byte length. off/len describe the selected range of that object;
// normal Window RMW publication produces a single SliceRecord per object.
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

// Validate one authoritative committed record, independent of request bounds.
// A malformed stored record is a data-integrity failure (EIO).
utils::Status ValidateRecord(const SliceRecord &record, uint32_t chunk_size);

// Materialize the visible spans for [pos, pos+len), including zero holes.
// Validates the *entire* source history, even records outside the requested
// range; invalid records return Malformed (EIO), while invalid caller inputs
// return InvalidArgument (EINVAL). Errors leave the caller's output untouched.
utils::Status ResolveRange(std::span<const SliceRecord> records, uint32_t chunk_size, uint32_t pos, uint32_t len,
                           std::vector<ResolvedSlice> *out);

}  // namespace swordfs::chunk::slice
