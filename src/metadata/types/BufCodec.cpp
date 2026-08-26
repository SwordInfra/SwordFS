// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/BufCodec.hpp"

#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <ctime>
#include <utility>

namespace swordfs::metadata {

namespace {

constexpr size_t kInitialBufferSize = 1024;
constexpr std::string_view kMagic = "SWFSMETA";
constexpr uint32_t kSchemaVersion = 1;

}  // namespace

class BufEncoder::Impl {
 public:
  Impl() : buffer(folly::IOBuf::create(kInitialBufferSize)), appender(buffer.get(), kInitialBufferSize) {
  }

  std::unique_ptr<folly::IOBuf> buffer;
  folly::io::Appender appender;
};

BufEncoder::BufEncoder() : impl_(std::make_unique<Impl>()) {
}

BufEncoder::~BufEncoder() = default;

BufEncoder::BufEncoder(BufEncoder &&) noexcept = default;

BufEncoder &BufEncoder::operator=(BufEncoder &&) noexcept = default;

void BufEncoder::U32(uint32_t value) {
  impl_->appender.writeLE<uint32_t>(value);
}

void BufEncoder::U64(uint64_t value) {
  impl_->appender.writeLE<uint64_t>(value);
}

void BufEncoder::String(std::string_view value) {
  U64(value.size());
  impl_->appender.push(reinterpret_cast<const uint8_t *>(value.data()), value.size());
}

void BufEncoder::Timespec(const struct timespec &ts) {
  U64(static_cast<uint64_t>(static_cast<int64_t>(ts.tv_sec)));
  U64(static_cast<uint64_t>(static_cast<int64_t>(ts.tv_nsec)));
}

void BufEncoder::Header(std::string_view magic, uint32_t schema_version) {
  String(magic);
  U32(schema_version);
}

void BufEncoder::Header(RecordType type) {
  Header(kMagic, kSchemaVersion);
  U32(static_cast<uint32_t>(type));
}

void BufEncoder::Finish(std::string *out) {
  impl_->buffer->appendTo(*out);
}

class BufDecoder::Impl {
 public:
  explicit Impl(std::string_view data)
      : buffer(folly::IOBuf::wrapBuffer(data.data(), data.size())), cursor(buffer.get()) {
  }

  std::unique_ptr<folly::IOBuf> buffer;
  folly::io::Cursor cursor;
};

BufDecoder::BufDecoder(std::string_view data) : impl_(std::make_unique<Impl>(data)) {
}

BufDecoder::~BufDecoder() = default;

BufDecoder::BufDecoder(BufDecoder &&) noexcept = default;

BufDecoder &BufDecoder::operator=(BufDecoder &&) noexcept = default;

bool BufDecoder::U32(uint32_t *value) {
  return impl_->cursor.tryReadLE(*value);
}

bool BufDecoder::U64(uint64_t *value) {
  return impl_->cursor.tryReadLE(*value);
}

bool BufDecoder::String(std::string *value) {
  uint64_t length = 0;
  if (!U64(&length) || !impl_->cursor.canAdvance(length)) {
    return false;
  }
  *value = impl_->cursor.readFixedString(length);
  return true;
}

bool BufDecoder::Timespec(struct timespec *ts) {
  uint64_t sec = 0;
  uint64_t nsec = 0;
  if (!U64(&sec) || !U64(&nsec)) {
    return false;
  }
  ts->tv_sec = static_cast<time_t>(static_cast<int64_t>(sec));
  ts->tv_nsec = static_cast<long>(static_cast<int64_t>(nsec));
  return ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L;
}

bool BufDecoder::Header(std::string_view expected_magic, uint32_t expected_schema_version) {
  std::string magic;
  uint32_t version = 0;
  return String(&magic) && magic == expected_magic && U32(&version) && version == expected_schema_version;
}

bool BufDecoder::Header(RecordType expected_type) {
  uint32_t type = 0;
  return Header(kMagic, kSchemaVersion) && U32(&type) && type == static_cast<uint32_t>(expected_type);
}

bool BufDecoder::Done() const {
  return impl_->cursor.isAtEnd();
}

}  // namespace swordfs::metadata
