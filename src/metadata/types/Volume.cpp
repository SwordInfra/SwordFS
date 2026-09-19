// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Volume.hpp"

#include <cstdint>
#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

std::string SwordFsVolume::SerializeTo() const {
  std::string out;
  BufEncoder enc;
  enc.Header(RecordType::kVolume);
  enc.String(name);
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
  dec.Header(RecordType::kVolume);
  dec.String(&volume.name);
  dec.String(&volume.storage);
  dec.String(&volume.bucket);
  dec.String(&volume.region);
  dec.U64(&volume.chunk_size);
  if (!dec || volume.name.empty() || volume.chunk_size == 0 || !dec.Done()) {
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
