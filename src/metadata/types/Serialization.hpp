// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata::types::serialization {

constexpr size_t kInitialBufferSize = 1024;
constexpr size_t kMaxStringLength = 16 * 1024 * 1024;
constexpr std::string_view kMagic = "SWFSMETA";
constexpr uint32_t kSchemaVersion = 1;

enum class RecordType : uint32_t {
  kInode = 1,
  kEntry = 2,
  kChunk = 3,
};

class Writer {
 public:
  Writer() : buffer_(folly::IOBuf::create(kInitialBufferSize)), appender_(buffer_.get(), kInitialBufferSize) {
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

  void Header(RecordType type) {
    String(kMagic);
    U32(kSchemaVersion);
    U32(static_cast<uint32_t>(type));
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
    if (!U64(&length) || length > kMaxStringLength || !cursor_.canAdvance(length)) {
      return false;
    }
    *value = cursor_.readFixedString(length);
    return true;
  }

  bool Header(RecordType expected_type) {
    std::string magic;
    uint32_t version = 0;
    uint32_t type = 0;
    return String(&magic) && magic == kMagic && U32(&version) && version == kSchemaVersion && U32(&type) &&
           type == static_cast<uint32_t>(expected_type);
  }

  bool Done() const {
    return cursor_.isAtEnd();
  }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Cursor cursor_;
};

inline utils::Status Malformed(std::string_view type) {
  return utils::Status::Malformed("Malformed metadata " + std::string(type) + " record");
}

inline void WriteTimespec(Writer &writer, const struct timespec &ts) {
  writer.U64(static_cast<uint64_t>(static_cast<int64_t>(ts.tv_sec)));
  writer.U64(static_cast<uint64_t>(static_cast<int64_t>(ts.tv_nsec)));
}

inline bool ReadTimespec(Reader &reader, struct timespec *ts) {
  uint64_t sec = 0;
  uint64_t nsec = 0;
  if (!reader.U64(&sec) || !reader.U64(&nsec)) {
    return false;
  }
  ts->tv_sec = static_cast<time_t>(static_cast<int64_t>(sec));
  ts->tv_nsec = static_cast<long>(static_cast<int64_t>(nsec));
  return ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L;
}

}  // namespace swordfs::metadata::types::serialization
