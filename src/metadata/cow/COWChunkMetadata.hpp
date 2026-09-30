// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "metadata/ChunkMetadata.hpp"

namespace swordfs::metadata::cow {

inline constexpr uint64_t kMaxCOWChunkRevisionValue = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

// Immutable physical-object version scoped to one ChunkID. Zero is invalid;
// different ChunkIDs may independently use the same revision value.
class COWChunkRevision {
 public:
  constexpr COWChunkRevision() = default;
  explicit constexpr COWChunkRevision(uint64_t value) : value_(value) {
  }

  auto operator<=>(const COWChunkRevision &) const = default;

  constexpr uint64_t Value() const {
    return value_;
  }

 private:
  uint64_t value_ = 0;
};

inline constexpr COWChunkRevision kInvalidCOWChunkRevision;

// Complete COW publication identity for one stable ChunkID. Size is the valid
// logical prefix of the immutable object identified by revision.
struct COWChunkHead {
  COWChunkRevision revision = kInvalidCOWChunkRevision;
  uint64_t size = 0;

  bool operator==(const COWChunkHead &) const = default;
};

namespace internal {

inline bool IsValidChunkID(ChunkID chunk_id) {
  return chunk_id != kInvalidChunkID && chunk_id.Value() <= kMaxChunkIDValue;
}

inline bool IsValidHead(const COWChunkHead &head) {
  return head.revision != kInvalidCOWChunkRevision && head.revision.Value() <= kMaxCOWChunkRevisionValue;
}

inline utils::Status ValidateHeadTransition(const std::optional<COWChunkHead> &expected,
                                            const COWChunkHead &replacement) {
  if (!IsValidHead(replacement) || (expected.has_value() && !IsValidHead(*expected))) {
    return utils::Status::InvalidArgument("invalid COW chunk head");
  }
  if (!expected.has_value()) {
    return utils::Status::OK();
  }
  if (replacement.revision.Value() < expected->revision.Value()) {
    return utils::Status::InvalidArgument("COW revision must not move backward");
  }
  if (replacement.revision == expected->revision && replacement.size > expected->size) {
    return utils::Status::InvalidArgument("same COW revision may only shrink its visible prefix");
  }
  return utils::Status::OK();
}

}  // namespace internal

// Typed root for COW-owned metadata. The interface is intentionally semantic:
// it exposes no raw backend fields and never participates in FileMetadata
// transactions.
class COWChunkMetadata : public ChunkMetadata {
 public:
  ChunkType Type() const final {
    return ChunkType::kCow;
  }

  virtual utils::Status AllocateRevision(ChunkID chunk_id, COWChunkRevision *out) = 0;
  virtual utils::Status GetHead(ChunkID chunk_id, COWChunkHead *out) = 0;
  virtual utils::Status CompareExchangeHead(ChunkID chunk_id, const std::optional<COWChunkHead> &expected,
                                            const COWChunkHead &replacement) = 0;
  virtual utils::Status EraseHead(ChunkID chunk_id, const COWChunkHead &expected) = 0;
};

using COWChunkMetadataPtr = std::shared_ptr<COWChunkMetadata>;

}  // namespace swordfs::metadata::cow
