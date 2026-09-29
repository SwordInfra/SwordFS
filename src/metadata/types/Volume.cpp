// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Volume.hpp"

#include <cstdint>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

bool IsKnownChunkType(ChunkType chunk_type) {
  switch (chunk_type) {
    case ChunkType::kCow:
    case ChunkType::kChunkSlice:
    case ChunkType::kRedisCache:
      return true;
  }
  return false;
}

std::string_view ChunkTypeName(ChunkType chunk_type) {
  switch (chunk_type) {
    case ChunkType::kCow:
      return "cow";
    case ChunkType::kChunkSlice:
      return "chunk_slice";
    case ChunkType::kRedisCache:
      return "redis_cache";
  }
  return {};
}

std::string ChunkTypeKey(ChunkType chunk_type) {
  return std::to_string(static_cast<uint32_t>(chunk_type));
}

utils::Status ParseChunkType(std::string_view name, ChunkType *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk type output is null");
  }
  for (const auto chunk_type : {ChunkType::kCow, ChunkType::kChunkSlice, ChunkType::kRedisCache}) {
    if (name == ChunkTypeName(chunk_type)) {
      *out = chunk_type;
      return utils::Status::OK();
    }
  }
  return utils::Status::InvalidArgument("unknown chunk type: " + std::string(name));
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
  enc.U32(static_cast<uint32_t>(chunk_type));
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
  uint32_t chunk_type = 0;
  dec.U32(&chunk_type);
  volume.chunk_type = static_cast<ChunkType>(chunk_type);
  if (!dec || volume.name.empty() || volume.chunk_size == 0 || !IsKnownChunkType(volume.chunk_type) || !dec.Done()) {
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
