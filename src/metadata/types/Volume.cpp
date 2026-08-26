// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Volume.hpp"

#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace swordfs::metadata {
namespace {
constexpr std::string_view kVolumeMagic = "SWORVOL1";
constexpr uint32_t kVolumeSchemaVersion = 1;

class Writer {
 public:
  Writer() : buffer_(folly::IOBuf::create(256)), appender_(buffer_.get(), 256) {
  }

  void U32(uint32_t value) {
    appender_.writeLE<uint32_t>(value);
  }
  void U64(uint64_t value) {
    appender_.writeLE<uint64_t>(value);
  }
  void String(std::string_view value) {
    U64(value.size());
    appender_.push(reinterpret_cast<const uint8_t *>(value.data()), value.size());
  }
  void Finish(std::string *out) {
    buffer_->appendTo(*out);
  }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Appender appender_;
};

class Reader {
 public:
  explicit Reader(std::string_view data)
      : buffer_(folly::IOBuf::wrapBuffer(data.data(), data.size())), cursor_(buffer_.get()) {
  }

  bool U32(uint32_t *value) {
    return cursor_.tryReadLE(*value);
  }
  bool U64(uint64_t *value) {
    return cursor_.tryReadLE(*value);
  }
  bool String(std::string *value) {
    uint64_t length = 0;
    if (!U64(&length) || length > 16 * 1024 * 1024 || !cursor_.canAdvance(length)) {
      return false;
    }
    *value = cursor_.readFixedString(length);
    return true;
  }
  bool Header() {
    std::string magic;
    uint32_t version = 0;
    return String(&magic) && magic == kVolumeMagic && U32(&version) && version == kVolumeSchemaVersion;
  }
  bool Done() const {
    return cursor_.isAtEnd();
  }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Cursor cursor_;
};
}  // namespace

std::string VolumeFormat::SerializeTo() const {
  std::string out;
  Writer writer;
  writer.String(kVolumeMagic);
  writer.U32(kVolumeSchemaVersion);
  writer.String(name);
  writer.String(meta_url);
  writer.String(storage);
  writer.String(bucket);
  writer.String(region);
  writer.U64(chunk_size);
  writer.Finish(&out);
  return out;
}

utils::Status VolumeFormat::ParseFrom(std::string_view data) {
  Reader reader(data);
  VolumeFormat volume;
  uint64_t chunk_size_value = 0;
  if (!reader.Header() || !reader.String(&volume.name) || !reader.String(&volume.meta_url) ||
      !reader.String(&volume.storage) || !reader.String(&volume.bucket) || !reader.String(&volume.region) ||
      !reader.U64(&chunk_size_value) || chunk_size_value == 0 ||
      chunk_size_value > std::numeric_limits<size_t>::max() || !reader.Done()) {
    return utils::Status::Malformed("Malformed volume metadata record");
  }
  volume.chunk_size = static_cast<size_t>(chunk_size_value);
  *this = std::move(volume);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
