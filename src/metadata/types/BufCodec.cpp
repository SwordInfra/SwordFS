// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/BufCodec.hpp"

#include "metadata/types/Inode.hpp"

#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <utility>

namespace swordfs::metadata {

namespace {

constexpr size_t kInitialBufferSize = 1024;
constexpr std::string_view kMagic = "SWFSMETA";
constexpr uint32_t kSchemaVersion = 1;

}  // namespace

class BufEncoder::Impl {
 public:
  Impl() : buffer_(folly::IOBuf::create(kInitialBufferSize)), appender_(buffer_.get(), kInitialBufferSize) {
  }

  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Appender appender_;
};

BufEncoder::BufEncoder() : impl_(std::make_unique<Impl>()) {
}

BufEncoder::~BufEncoder() = default;

BufEncoder::BufEncoder(BufEncoder &&) noexcept = default;

BufEncoder &BufEncoder::operator=(BufEncoder &&) noexcept = default;

void BufEncoder::U32(uint32_t value) {
  impl_->appender_.writeLE<uint32_t>(value);
}

void BufEncoder::U64(uint64_t value) {
  impl_->appender_.writeLE<uint64_t>(value);
}

void BufEncoder::I64(int64_t value) {
  impl_->appender_.writeLE<int64_t>(value);
}

void BufEncoder::String(std::string_view value) {
  U64(value.size());
  impl_->appender_.push(reinterpret_cast<const uint8_t *>(value.data()), value.size());
}

void BufEncoder::Attr(const SwordFsAttr &attr) {
  U64(attr.dev);
  U64(attr.ino);
  U32(attr.mode);
  U64(attr.nlink);
  U64(attr.uid);
  U64(attr.gid);
  U64(attr.rdev);
  U64(attr.size);
  U64(attr.blksize);
  U64(attr.blocks);
  I64(attr.atime);
  I64(attr.atime_nsec);
  I64(attr.mtime);
  I64(attr.mtime_nsec);
  I64(attr.ctime);
  I64(attr.ctime_nsec);
}

void BufEncoder::Header(RecordType type) {
  String(kMagic);
  U32(kSchemaVersion);
  U32(static_cast<uint32_t>(type));
}

void BufEncoder::Finish(std::string *out) {
  if (out == nullptr) {
    return;
  }
  out->clear();
  impl_->buffer_->appendTo(*out);
}

class BufDecoder::Impl {
 public:
  explicit Impl(std::string_view data)
      : buffer_(folly::IOBuf::wrapBuffer(data.data(), data.size())), cursor_(buffer_.get()) {
  }

  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Cursor cursor_;
  bool failed_{false};
};

BufDecoder::BufDecoder(std::string_view data) : impl_(std::make_unique<Impl>(data)) {
}

BufDecoder::~BufDecoder() = default;

BufDecoder::BufDecoder(BufDecoder &&) noexcept = default;

BufDecoder &BufDecoder::operator=(BufDecoder &&) noexcept = default;

bool BufDecoder::U32(uint32_t *value) {
  const bool ok = impl_->cursor_.tryReadLE(*value);
  impl_->failed_ |= !ok;
  return ok;
}

bool BufDecoder::U64(uint64_t *value) {
  const bool ok = impl_->cursor_.tryReadLE(*value);
  impl_->failed_ |= !ok;
  return ok;
}

bool BufDecoder::I64(int64_t *value) {
  const bool ok = impl_->cursor_.tryReadLE(*value);
  impl_->failed_ |= !ok;
  return ok;
}

bool BufDecoder::String(std::string *value) {
  uint64_t length = 0;
  if (!U64(&length) || value == nullptr || !impl_->cursor_.canAdvance(length)) {
    impl_->failed_ = true;
    return false;
  }
  *value = impl_->cursor_.readFixedString(length);
  return true;
}

bool BufDecoder::Attr(SwordFsAttr *attr) {
  if (attr == nullptr) {
    impl_->failed_ = true;
    return false;
  }
  if (!U64(&attr->dev) || !U64(&attr->ino) || !U32(&attr->mode) || !U64(&attr->nlink) ||
      !U64(&attr->uid) || !U64(&attr->gid) || !U64(&attr->rdev) || !U64(&attr->size) ||
      !U64(&attr->blksize) || !U64(&attr->blocks) || !I64(&attr->atime) ||
      !I64(&attr->atime_nsec) || !I64(&attr->mtime) || !I64(&attr->mtime_nsec) ||
      !I64(&attr->ctime) || !I64(&attr->ctime_nsec)) {
    return false;
  }
  if (attr->atime_nsec < 0 || attr->atime_nsec >= 1000000000 ||
      attr->mtime_nsec < 0 || attr->mtime_nsec >= 1000000000 ||
      attr->ctime_nsec < 0 || attr->ctime_nsec >= 1000000000) {
    impl_->failed_ = true;
    return false;
  }
  return true;
}

bool BufDecoder::Header(RecordType expected_type) {
  std::string magic;
  uint32_t version = 0;
  uint32_t type = 0;
  if (!String(&magic) || magic != kMagic || !U32(&version) ||
      version != kSchemaVersion || !U32(&type) ||
      type != static_cast<uint32_t>(expected_type)) {
    impl_->failed_ = true;
    return false;
  }
  return true;
}

bool BufDecoder::Done() const {
  return !impl_->failed_ && impl_->cursor_.isAtEnd();
}

BufDecoder::operator bool() const noexcept {
  return !impl_->failed_;
}

}  // namespace swordfs::metadata
