// Copyright 2026 SwordFS Contributors.
// License
//
// Zero-copy stream helpers for AWS SDK I/O in S3DataEngine.
//
// Goal: eliminate intermediate std::string / memcpy when moving
// data between folly::IOBuf and the AWS SDK's HTTP layer.
//
//   • Put:  SDK reads  from our IOBuf → use PreallocatedStreamBuf
//           (already provided by AWS SDK).
//   • Get:  SDK writes into our IOBuf → use PreallocatedOutputStreamBuf
//           + SetResponseStreamFactory to redirect the response body.
//

#pragma once

#include <aws/core/Aws.h>

#include <cstddef>
#include <ios>
#include <streambuf>

namespace swordfs::storage {

/// A std::streambuf that writes into a caller-provided buffer.
/// Unlike PreallocatedStreamBuf (read-only, in
/// <aws/.../PreallocatedStreamBuf.h>), this exposes the put area so
/// the SDK's ostream layer writes directly into the target.
class PreallocatedOutputStreamBuf : public std::streambuf {
 public:
  PreallocatedOutputStreamBuf(char *buffer, size_t capacity);
  /// Number of bytes actually written into the buffer.
  size_t Written() const;

 protected:
  std::streamsize xsputn(const char *s, std::streamsize n) override;
  int_type overflow(int_type ch) override;
  pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                   std::ios_base::openmode which) override;
  int sync() override;
};

/// A self-contained Aws::IOStream that owns a PreallocatedOutputStreamBuf.
/// Instances are created by the response-stream factory and destroyed by
/// the SDK via Aws::Delete, which correctly tears down both the stream
/// and its streambuf.
class PreallocatedResponseStream : public Aws::IOStream {
 public:
  PreallocatedResponseStream(char *buffer, size_t capacity);
  size_t Written() const;

 private:
  PreallocatedOutputStreamBuf buf_;
};

}  // namespace swordfs::storage