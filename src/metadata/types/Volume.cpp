// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Volume.hpp"

#include <cstdint>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {
namespace {
constexpr std::string_view kVolumeMagic = "SWORVOL1";
constexpr uint32_t kVolumeSchemaVersion = 1;
}  // namespace

std::string SwordFsVolume::SerializeTo() const {
  std::string out;
  BufEncoder enc;
  enc.Header(kVolumeMagic, kVolumeSchemaVersion);
  enc.String(name);
  enc.String(meta_url);
  enc.String(storage);
  enc.String(bucket);
  enc.String(region);
  enc.U64(chunk_size);
  enc.Finish(&out);
  return out;
}

utils::Status SwordFsVolume::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  SwordFsVolume volume;
  dec.Header(kVolumeMagic, kVolumeSchemaVersion);
  dec.String(&volume.name);
  dec.String(&volume.meta_url);
  dec.String(&volume.storage);
  dec.String(&volume.bucket);
  dec.String(&volume.region);
  dec.U64(&volume.chunk_size);
  if (!dec || volume.name.empty() || volume.chunk_size == 0 || !dec.Done()) {
    return utils::Status::Malformed("Malformed volume metadata record");
  }
  *this = std::move(volume);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
