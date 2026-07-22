// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// S3DataEngine — object-storage backend via AWS SDK for C++.
//
// Controlled by CMake option ENABLE_S3=ON/OFF.  Builds only when
// libaws-cpp-sdk-s3 is available.
//
// Chunks are stored as immutable objects under a configurable bucket
// and prefix.

#pragma once

#include <aws/s3/S3Client.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "storage/IDataEngine.hpp"

namespace swordfs::storage {

struct S3Config {
  std::string endpoint;   // e.g. "https://s3.amazonaws.com"
  std::string region;     // e.g. "us-east-1"
  std::string bucket;
  std::string prefix;     // optional key prefix, e.g. "swordfs/chunks"
};

/// S3-compatible object storage engine using AWS SDK for C++.
///
/// Authentication is handled by the SDK's default credential chain
/// (environment, ~/.aws/credentials, IAM role).
class S3DataEngine : public IDataEngine {
 public:
  explicit S3DataEngine(const S3Config& config);
  ~S3DataEngine() override = default;

  Status Put(std::string_view key, std::string_view data) override;
  Status Get(std::string_view key, std::string* out,
             size_t offset = 0, size_t size = 0) override;
  Status Delete(std::string_view key) override;

  /// Return the S3 object key for a chunk identifier.
  /// Format: "<prefix>/<key>" (or just "<key>" if prefix is empty).
  std::string ObjectKey(std::string_view key) const;

 private:
  S3Config cfg_;
  std::unique_ptr<Aws::S3::S3Client> client_;
  mutable std::mutex mu_;  // S3Client is not thread-safe
};

}  // namespace swordfs::storage
