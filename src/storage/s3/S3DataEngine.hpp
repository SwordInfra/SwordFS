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
class BlockingExecutor;
}

namespace storage {

/// S3-compatible object storage engine using AWS SDK for C++.
class S3DataEngine : public IDataEngine {
 public:
  static Status CreateInstance(std::unique_ptr<IDataEngine> *out);

  S3DataEngine();
  ~S3DataEngine() override;

  Status Initialize();

  DataEngineLimits Limits() const override;
  bool Head(std::string_view key, size_t *size) override;
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override;
  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) override;
  Status Delete(std::string_view key) override;

  /// Return the S3 object key for a chunk identifier.
  /// Format: "<prefix>/<key>" (or just "<key>" if prefix is empty).
  std::string ObjectKey(std::string_view key) const;

 private:
  Status ParseBucketUrl();

 private:
  std::string endpoint_;
  std::string region_;
  std::string bucket_;
  std::string prefix_;
  std::unique_ptr<swordfs::utils::BlockingExecutor> executor_;
  std::unique_ptr<Aws::S3::S3Client> client_;
};

}  // namespace storage
}  // namespace swordfs
