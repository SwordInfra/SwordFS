// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Volume.hpp"

#include <cstdint>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

bool IsKnownChunkOverwriteMechanism(ChunkOverwriteMechanism mechanism) {
  switch (mechanism) {
    case ChunkOverwriteMechanism::kWholeObject:
    case ChunkOverwriteMechanism::kChunkSlice:
    case ChunkOverwriteMechanism::kRedisCache:
      return true;
  }
  return false;
}

std::string_view ChunkOverwriteMechanismName(ChunkOverwriteMechanism mechanism) {
  switch (mechanism) {
    case ChunkOverwriteMechanism::kWholeObject:
      return "whole_object";
    case ChunkOverwriteMechanism::kChunkSlice:
      return "chunk_slice";
    case ChunkOverwriteMechanism::kRedisCache:
      return "redis_cache";
  }
  return {};
}

std::string ChunkOverwriteMechanismKey(ChunkOverwriteMechanism mechanism) {
  return std::to_string(static_cast<uint32_t>(mechanism));
}

utils::Status ParseChunkOverwriteMechanism(std::string_view name, ChunkOverwriteMechanism *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk overwrite mechanism output is null");
  }
  for (const auto mechanism : {ChunkOverwriteMechanism::kWholeObject, ChunkOverwriteMechanism::kChunkSlice,
                               ChunkOverwriteMechanism::kRedisCache}) {
    if (name == ChunkOverwriteMechanismName(mechanism)) {
      *out = mechanism;
      return utils::Status::OK();
    }
  }
  return utils::Status::InvalidArgument("unknown chunk overwrite mechanism: " + std::string(name));
}

std::string SwordFsVolume::SerializeTo() const {
  std::string out;
  BufEncoder enc;
  enc.Header(RecordType::kVolume);
  enc.String(name);
  enc.String(storage);
  enc.String(bucket);
  enc.String(region);
  enc.U64(chunk_size);
  enc.U32(static_cast<uint32_t>(chunk_overwrite_mechanism));
  enc.U32(chunk_index_format_version);
  enc.Finish(&out);
  return out;
}

utils::Status SwordFsVolume::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  SwordFsVolume volume;
  dec.Header(RecordType::kVolume);
  dec.String(&volume.name);
  dec.String(&volume.storage);
  dec.String(&volume.bucket);
  dec.String(&volume.region);
  dec.U64(&volume.chunk_size);
  uint32_t chunk_overwrite_mechanism = 0;
  dec.U32(&chunk_overwrite_mechanism);
  volume.chunk_overwrite_mechanism = static_cast<ChunkOverwriteMechanism>(chunk_overwrite_mechanism);
  dec.U32(&volume.chunk_index_format_version);
  if (!dec || volume.name.empty() || volume.chunk_size == 0 ||
      !IsKnownChunkOverwriteMechanism(volume.chunk_overwrite_mechanism) || volume.chunk_index_format_version == 0 ||
      !dec.Done()) {
    return utils::Status::Malformed("Malformed volume metadata record");
  }
  const bool has_data_engine = !volume.storage.empty();
  const bool has_data_location = !volume.bucket.empty();
  if (has_data_engine != has_data_location) {
    return utils::Status::Malformed("Malformed volume metadata record");
  }
  *this = std::move(volume);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
