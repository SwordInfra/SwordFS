// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Volume.hpp"

#include <cstdint>
#include <limits>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {
namespace {
constexpr std::string_view kVolumeMagic = "SWORVOL1";
constexpr uint32_t kVolumeSchemaVersion = 1;
}  // namespace

std::string SwordFsVolume::SerializeTo() const {
  std::string out;
  BufEncoder writer;
  writer.Header(kVolumeMagic, kVolumeSchemaVersion);
  writer.String(name);
  writer.String(meta_url);
  writer.String(storage);
  writer.String(bucket);
  writer.String(region);
  writer.U64(chunk_size);
  writer.Finish(&out);
  return out;
}

utils::Status SwordFsVolume::ParseFrom(std::string_view data) {
  BufDecoder reader(data);
  SwordFsVolume volume;
  uint64_t chunk_size_value = 0;
  reader.Header(kVolumeMagic, kVolumeSchemaVersion);
  reader.String(&volume.name);
  reader.String(&volume.meta_url);
  reader.String(&volume.storage);
  reader.String(&volume.bucket);
  reader.String(&volume.region);
  reader.U64(&chunk_size_value);
  if (!reader || volume.name.empty() || chunk_size_value == 0 ||
      chunk_size_value > std::numeric_limits<size_t>::max() || !reader.Done()) {
    return utils::Status::Malformed("Malformed volume metadata record");
  }
  volume.chunk_size = static_cast<size_t>(chunk_size_value);
  *this = std::move(volume);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
