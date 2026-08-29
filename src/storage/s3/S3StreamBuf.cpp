// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Method bodies for the stream helpers declared in S3StreamBuf.hpp.

#include "storage/s3/S3StreamBuf.hpp"

#include <algorithm>
#include <cstring>

namespace swordfs::storage {

// ─── PreallocatedOutputStreamBuf ────────────────────────────────

PreallocatedOutputStreamBuf::PreallocatedOutputStreamBuf(char *buffer, size_t capacity) {
  setp(buffer, buffer + capacity);
}

size_t PreallocatedOutputStreamBuf::Written() const {
  return static_cast<size_t>(pptr() - pbase());
}

std::streamsize PreallocatedOutputStreamBuf::xsputn(const char *s, std::streamsize n) {
  auto avail = static_cast<std::streamsize>(epptr() - pptr());
  auto actual = std::min(n, avail);
  std::memcpy(pptr(), s, static_cast<size_t>(actual));
  pbump(static_cast<int>(actual));
  return actual;
}

std::streambuf::int_type PreallocatedOutputStreamBuf::overflow(int_type ch) {
  return traits_type::eof();
}

std::streambuf::pos_type PreallocatedOutputStreamBuf::seekoff(off_type off, std::ios_base::seekdir dir,
                                                              std::ios_base::openmode which) {
  if (which == std::ios_base::out && off == 0 && dir == std::ios_base::cur) {
    return pptr() - pbase();
  }
  return pos_type(off_type(-1));
}

int PreallocatedOutputStreamBuf::sync() {
  return std::streambuf::sync();
}

// ─── PreallocatedResponseStream ────────────────────────────────

PreallocatedResponseStream::PreallocatedResponseStream(char *buffer, size_t capacity)
    : Aws::IOStream(&buf_), buf_(buffer, capacity) {
}

size_t PreallocatedResponseStream::Written() const {
  return buf_.Written();
}

}  // namespace swordfs::storage
