// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// S3DataEngine — object-storage backend via AWS SDK for C++.
//
// Chunks are stored as immutable objects under a configurable bucket
// and prefix.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "storage/IDataEngine.hpp"

namespace Aws {
namespace S3 {
class S3Client;
}
}  // namespace Aws

namespace swordfs {

namespace utils {
class FiberThreadPool;
}

namespace storage {

struct S3Config {
  std::string endpoint;  // e.g. "https://s3.amazonaws.com"
  std::string region;    // e.g. "us-east-1"
  std::string bucket;
  std::string prefix;  // optional key prefix, e.g. "swordfs/chunks"
};

/// S3-compatible object storage engine using AWS SDK for C++.
///
/// Authentication is handled by the SDK's default credential chain
/// (environment, ~/.aws/credentials, IAM role).
///
/// Thread safety: the AWS SDK S3Client is internally thread-safe
/// (connection pool, default 25 connections).  No external locking
/// is needed.
class S3DataEngine : public IDataEngine {
 public:
  explicit S3DataEngine(const S3Config& config);
  ~S3DataEngine() override;

  DataEngineLimits Limits() const override;
  bool Head(std::string_view key, size_t* size) override;
  Status Put(std::string_view key,
             std::unique_ptr<folly::IOBuf> data) override;
  Status Get(std::string_view key, size_t offset, size_t size,
             folly::IOBuf* out) override;
  Status Delete(std::string_view key) override;

  /// Return the S3 object key for a chunk identifier.
  /// Format: "<prefix>/<key>" (or just "<key>" if prefix is empty).
  std::string ObjectKey(std::string_view key) const;

 private:
  S3Config cfg_;
  std::shared_ptr<swordfs::utils::FiberThreadPool> pool_;
  std::unique_ptr<Aws::S3::S3Client> client_;
};

}  // namespace storage
}  // namespace swordfs
