// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisSliceChunkMetadata.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/types/BufCodec.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {
namespace {

constexpr size_t kEncodedSliceRecordSize = 24;

bool IsValidChunkID(ChunkID chunk_id) {
  return chunk_id != kInvalidChunkID && chunk_id.Value() <= kMaxChunkIDValue;
}

std::string EncodeRecord(const chunk::slice::SliceRecord &record) {
  BufEncoder encoder;
  encoder.U32(record.pos);
  encoder.U64(record.id.Value());
  encoder.U32(record.size);
  encoder.U32(record.off);
  encoder.U32(record.len);
  std::string encoded;
  encoder.Finish(&encoded);
  return encoded;
}

utils::Status DecodeRecord(std::string_view encoded, uint32_t chunk_size, chunk::slice::SliceRecord *out) {
  if (encoded.size() != kEncodedSliceRecordSize) {
    return utils::Status::Malformed("invalid length of persisted Slice record");
  }
  BufDecoder decoder(encoded);
  chunk::slice::SliceRecord record;
  uint64_t slice_id = 0;
  if (!decoder.U32(&record.pos) || !decoder.U64(&slice_id) || !decoder.U32(&record.size) || !decoder.U32(&record.off) ||
      !decoder.U32(&record.len) || !decoder.Done()) {
    return utils::Status::Malformed("invalid binary Slice record");
  }
  record.id = chunk::slice::SliceID(slice_id);
  auto status = chunk::slice::ValidateRecord(record, chunk_size);
  if (!status.ok()) {
    return status;
  }
  *out = record;
  return utils::Status::OK();
}

}  // namespace

utils::Status RedisSliceChunkMetadata::AllocateChunkID(ChunkID *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("ChunkID output is null");
  }
  uint64_t value = 0;
  auto status = backend_->executor().RunFromFiber(
      [&] { return backend_->client().IncrExistingNonNegative(key_.NextChunkID(), &value); });
  if (!status.ok()) {
    return status;
  }
  *out = ChunkID(value);
  return utils::Status::OK();
}

utils::Status RedisSliceChunkMetadata::AllocateSliceID(chunk::slice::SliceID *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("SliceID output is null");
  }
  uint64_t value = 0;
  auto status = backend_->executor().RunFromFiber(
      [&] { return backend_->client().IncrExistingNonNegative(key_.NextSliceID(), &value); });
  if (!status.ok()) {
    return status;
  }
  *out = chunk::slice::SliceID(value);
  return utils::Status::OK();
}

utils::Status RedisSliceChunkMetadata::CreateIndex(ChunkID chunk_id, const chunk::slice::SliceRecord &first_record) {
  utils::ExpectInFiberDomain();
  if (!IsValidChunkID(chunk_id)) {
    return utils::Status::InvalidArgument("invalid Slice ChunkID");
  }
  auto status = chunk::slice::ValidateRecord(first_record, chunk_size_);
  if (!status.ok()) {
    return utils::Status::InvalidArgument(status.message());
  }
  const auto key = IndexKey(chunk_id);
  const auto encoded = EncodeRecord(first_record);
  return backend_->executor().RunFromFiber([&] { return backend_->client().CreateListWithFirstRecord(key, encoded); });
}

utils::Status RedisSliceChunkMetadata::ReadRecords(ChunkID chunk_id, std::vector<chunk::slice::SliceRecord> *out) {
  utils::ExpectInFiberDomain();
  if (!IsValidChunkID(chunk_id) || out == nullptr) {
    return utils::Status::InvalidArgument("invalid Slice index read request");
  }
  std::vector<std::string> encoded;
  const auto key = IndexKey(chunk_id);
  auto status = backend_->executor().RunFromFiber([&] { return backend_->client().LRange(key, &encoded); });
  if (!status.ok()) {
    return status;
  }
  if (encoded.empty()) {
    return utils::Status::NotFound("Slice private index is missing");
  }
  std::vector<chunk::slice::SliceRecord> records;
  records.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    chunk::slice::SliceRecord record;
    status = DecodeRecord(encoded[i], chunk_size_, &record);
    if (!status.ok()) {
      return status;
    }
    records.push_back(record);
  }
  *out = std::move(records);
  return utils::Status::OK();
}

utils::Status RedisSliceChunkMetadata::AppendRecord(ChunkID chunk_id, const chunk::slice::SliceRecord &record) {
  utils::ExpectInFiberDomain();
  if (!IsValidChunkID(chunk_id)) {
    return utils::Status::InvalidArgument("invalid Slice ChunkID");
  }
  auto status = chunk::slice::ValidateRecord(record, chunk_size_);
  if (!status.ok()) {
    // This is a caller-provided pending record, not corrupted persisted data.
    return utils::Status::InvalidArgument(status.message());
  }
  const auto key = IndexKey(chunk_id);
  const auto encoded = EncodeRecord(record);
  return backend_->executor().RunFromFiber([&] { return backend_->client().RPushExisting(key, encoded); });
}

std::string RedisSliceChunkMetadata::IndexKey(ChunkID chunk_id) const {
  return key_.PrivateChunkIndex(ChunkType::kChunkSlice, "index:" + std::to_string(chunk_id.Value()));
}

}  // namespace swordfs::metadata
