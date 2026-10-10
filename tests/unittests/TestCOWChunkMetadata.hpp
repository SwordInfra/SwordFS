// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "metadata/cow/COWChunkMetadata.hpp"

namespace swordfs::test {

// Deliberately small typed metadata double for isolated chunk/GC tests. It
// models only identity allocation and the observable COW head CAS contract;
// it is not an IMetaEngine, namespace store or durable transaction backend.
class TestCOWChunkMetadata final : public metadata::cow::COWChunkMetadata {
 public:
  utils::Status AllocateChunkID(metadata::ChunkID *out) override {
    if (out == nullptr) {
      return utils::Status::InvalidArgument("chunk id output is null");
    }
    *out = metadata::ChunkID(++next_chunk_id_);
    return utils::Status::OK();
  }

  utils::Status AllocateRevision(metadata::ChunkID chunk_id, metadata::cow::COWChunkRevision *out) override {
    if (!metadata::cow::internal::IsValidChunkID(chunk_id) || out == nullptr) {
      return utils::Status::InvalidArgument("invalid COW revision request");
    }
    *out = metadata::cow::COWChunkRevision(++revisions_[chunk_id.Value()]);
    return utils::Status::OK();
  }

  utils::Status GetHead(metadata::ChunkID chunk_id, metadata::cow::COWChunkHead *out) override {
    if (!metadata::cow::internal::IsValidChunkID(chunk_id) || out == nullptr) {
      return utils::Status::InvalidArgument("invalid COW head request");
    }
    auto found = heads_.find(chunk_id.Value());
    if (found == heads_.end()) {
      return utils::Status::NotFound("no typed head");
    }
    *out = found->second;
    return utils::Status::OK();
  }

  utils::Status CompareExchangeHead(metadata::ChunkID chunk_id,
                                    const std::optional<metadata::cow::COWChunkHead> &expected,
                                    const metadata::cow::COWChunkHead &replacement) override {
    if (!metadata::cow::internal::IsValidChunkID(chunk_id)) {
      return utils::Status::InvalidArgument("invalid chunk id");
    }
    auto status = metadata::cow::internal::ValidateHeadTransition(expected, replacement);
    if (!status.ok()) {
      return status;
    }
    auto found = heads_.find(chunk_id.Value());
    if (found == heads_.end()) {
      if (expected.has_value()) {
        return utils::Status::NotFound("no typed head");
      }
      heads_.emplace(chunk_id.Value(), replacement);
      return utils::Status::OK();
    }
    if (!expected.has_value() || found->second != *expected) {
      return utils::Status::AlreadyExists("typed head changed");
    }
    found->second = replacement;
    return utils::Status::OK();
  }

  utils::Status EraseHead(metadata::ChunkID chunk_id, const metadata::cow::COWChunkHead &expected) override {
    if (!metadata::cow::internal::IsValidChunkID(chunk_id) || !metadata::cow::internal::IsValidHead(expected)) {
      return utils::Status::InvalidArgument("invalid COW erase request");
    }
    auto found = heads_.find(chunk_id.Value());
    if (found == heads_.end()) {
      return utils::Status::NotFound("no typed head");
    }
    if (found->second != expected) {
      return utils::Status::AlreadyExists("typed head changed");
    }
    heads_.erase(found);
    return utils::Status::OK();
  }

 private:
  uint64_t next_chunk_id_ = 0;
  std::map<uint64_t, uint64_t> revisions_;
  std::map<uint64_t, metadata::cow::COWChunkHead> heads_;
};

}  // namespace swordfs::test
