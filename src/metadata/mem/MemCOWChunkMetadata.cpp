// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemCOWChunkMetadata.hpp"

#include <mutex>

#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

utils::Status MemCOWChunkMetadata::AllocateChunkID(ChunkID *out) {
  utils::ExpectInFiberDomain();
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  return internal::AllocateChunkIDValue(&next_chunk_id_, out);
}

utils::Status MemCOWChunkMetadata::AllocateRevision(ChunkID chunk_id, cow::COWChunkRevision *out) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id) || out == nullptr) {
    return utils::Status::InvalidArgument("invalid COW revision allocation request");
  }
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto &current = next_revisions_[chunk_id.Value()];
  if (current >= cow::kMaxCOWChunkRevisionValue) {
    return utils::Status::IOError("COW chunk revision allocator exhausted");
  }
  ++current;
  *out = cow::COWChunkRevision(current);
  return utils::Status::OK();
}

utils::Status MemCOWChunkMetadata::GetHead(ChunkID chunk_id, cow::COWChunkHead *out) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id) || out == nullptr) {
    return utils::Status::InvalidArgument("invalid COW head read request");
  }
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  const auto it = heads_.find(chunk_id.Value());
  if (it == heads_.end()) {
    return utils::Status::NotFound("COW chunk head not found");
  }
  *out = it->second;
  return utils::Status::OK();
}

utils::Status MemCOWChunkMetadata::CompareExchangeHead(ChunkID chunk_id,
                                                       const std::optional<cow::COWChunkHead> &expected,
                                                       const cow::COWChunkHead &replacement) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id)) {
    return utils::Status::InvalidArgument("invalid COW ChunkID");
  }
  auto status = cow::internal::ValidateHeadTransition(expected, replacement);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<utils::FiberMutex> lock(mutex_);
  const auto it = heads_.find(chunk_id.Value());
  if (it == heads_.end()) {
    if (expected.has_value()) {
      return utils::Status::NotFound("COW chunk head not found");
    }
    heads_.emplace(chunk_id.Value(), replacement);
    return utils::Status::OK();
  }
  if (!expected.has_value() || it->second != *expected) {
    return utils::Status::AlreadyExists("COW chunk head changed before publication");
  }
  it->second = replacement;
  return utils::Status::OK();
}

utils::Status MemCOWChunkMetadata::EraseHead(ChunkID chunk_id, const cow::COWChunkHead &expected) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id) || !cow::internal::IsValidHead(expected)) {
    return utils::Status::InvalidArgument("invalid COW head erase request");
  }
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  const auto it = heads_.find(chunk_id.Value());
  if (it == heads_.end()) {
    return utils::Status::NotFound("COW chunk head not found");
  }
  if (it->second != expected) {
    return utils::Status::AlreadyExists("COW chunk head changed before erase");
  }
  heads_.erase(it);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
