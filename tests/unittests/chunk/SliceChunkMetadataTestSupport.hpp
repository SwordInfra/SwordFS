// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

// Shared in-memory Slice private index for Chunk and FileReadWriter tests.
// Failure injection remains confined to the test double, not production.

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "chunk/slice/SliceRecord.hpp"
#include "metadata/slice/SliceChunkMetadata.hpp"
#include "utils/Status.hpp"

namespace swordfs::test::chunk_test {

using swordfs::utils::Status;

class TestSliceChunkMetadata final : public swordfs::metadata::slice::SliceChunkMetadata {
 public:
  using Record = swordfs::chunk::slice::SliceRecord;
  Status AllocateChunkID(swordfs::metadata::ChunkID *out) override {
    if (!chunk_allocation_status_.ok()) {
      return chunk_allocation_status_;
    }
    if (allocate_zero_chunk_id_) {
      *out = swordfs::metadata::kInvalidChunkID;
      return Status::OK();
    }
    *out = swordfs::metadata::ChunkID(next_chunk_id_++);
    return Status::OK();
  }
  Status AllocateSliceID(swordfs::chunk::slice::SliceID *out) override {
    if (!allocation_status_.ok()) {
      return allocation_status_;
    }
    if (allocate_zero_id_) {
      *out = swordfs::chunk::slice::SliceID();
      return Status::OK();
    }
    *out = swordfs::chunk::slice::SliceID(next_slice_id_++);
    return Status::OK();
  }
  Status CreateIndex(swordfs::metadata::ChunkID id, const Record &record) override {
    if (!next_create_status_.ok()) {
      auto status = next_create_status_;
      next_create_status_ = Status::OK();
      return status;
    }
    if (!records_.emplace(id.Value(), std::vector<Record>{record}).second) {
      return Status::AlreadyExists("Slice private index already exists");
    }
    if (create_then_outcome_unknown_) {
      create_then_outcome_unknown_ = false;
      return Status::OutcomeUnknown("first record committed but reply was lost");
    }
    return Status::OK();
  }
  Status ReadRecords(swordfs::metadata::ChunkID id, std::vector<Record> *out) override {
    if (read_error_.has_value()) {
      return *read_error_;
    }
    const auto it = records_.find(id.Value());
    if (it == records_.end()) {
      return Status::NotFound("missing Slice index");
    }
    *out = it->second;
    return Status::OK();
  }
  Status AppendRecord(swordfs::metadata::ChunkID id, const Record &record) override {
    ++append_attempts_;
    if (fail_append_on_attempt_ == append_attempts_) {
      fail_append_on_attempt_.reset();
      return Status::IOError("second frozen candidate append failed");
    }
    if (!next_append_status_.ok()) {
      auto status = next_append_status_;
      next_append_status_ = Status::OK();
      return status;
    }
    const auto it = records_.find(id.Value());
    if (it == records_.end()) {
      return Status::NotFound("RPUSHX requires an existing Slice private index");
    }
    if (append_mismatched_size_then_outcome_unknown_) {
      append_mismatched_size_then_outcome_unknown_ = false;
      auto different = record;
      ++different.size;
      it->second.push_back(different);
      return Status::OutcomeUnknown("Redis persisted a record with different physical size");
    }
    if (append_mismatched_offset_then_outcome_unknown_) {
      append_mismatched_offset_then_outcome_unknown_ = false;
      auto different = record;
      different.off = 1;
      --different.len;
      it->second.push_back(different);
      return Status::OutcomeUnknown("Redis persisted a different but valid physical subrange");
    }
    if (append_mismatched_record_then_outcome_unknown_) {
      append_mismatched_record_then_outcome_unknown_ = false;
      auto different = record;
      different.len -= 1;
      it->second.push_back(different);
      return Status::OutcomeUnknown("Redis returned uncertain status for an unequal record");
    }
    it->second.push_back(record);
    if (append_then_outcome_unknown_) {
      append_then_outcome_unknown_ = false;
      return Status::OutcomeUnknown("Redis appended, but reply was lost");
    }
    return Status::OK();
  }
  std::optional<Status> read_error_;
  Status allocation_status_ = Status::OK();
  Status chunk_allocation_status_ = Status::OK();
  bool allocate_zero_chunk_id_ = false;
  Status next_create_status_ = Status::OK();
  Status next_append_status_ = Status::OK();
  bool create_then_outcome_unknown_ = false;
  bool append_then_outcome_unknown_ = false;
  bool append_mismatched_size_then_outcome_unknown_ = false;
  bool append_mismatched_offset_then_outcome_unknown_ = false;
  bool append_mismatched_record_then_outcome_unknown_ = false;
  size_t append_attempts_ = 0;
  std::optional<size_t> fail_append_on_attempt_;
  bool allocate_zero_id_ = false;

 private:
  std::map<uint64_t, std::vector<Record>> records_;
  uint64_t next_chunk_id_ = 1000;
  uint64_t next_slice_id_ = 1000;
};

}  // namespace swordfs::test::chunk_test
