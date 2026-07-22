// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// S3DataEngine — object-storage backend via AWS S3 API.
//
// Chunks are stored as immutable objects under a configurable bucket
// and prefix.  Multipart upload is used for chunks larger than the
// single-put threshold.
//
// Authentication: AWS Signature V4 via per-request signing.
// Transport: libcurl (HTTP/1.1).

#pragma once

#include <curl/curl.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "storage/IDataEngine.hpp"

namespace swordfs::storage {

struct S3Config {
  std::string endpoint;       // e.g. "s3.amazonaws.com" or "https://s3.amazonaws.com"
  std::string region;         // e.g. "us-east-1"
  std::string bucket;
  std::string prefix;         // optional key prefix, e.g. "swordfs/chunks"
  std::string access_key;
  std::string secret_key;
};

/// S3-compatible object storage engine.
class S3DataEngine : public IDataEngine {
 public:
  explicit S3DataEngine(const S3Config& config);
  ~S3DataEngine() override;

  Status Put(std::string_view key, std::string_view data) override;
  Status Get(std::string_view key, std::string* out,
             size_t offset = 0, size_t size = 0) override;
  Status Delete(std::string_view key) override;

 private:
  // Build the full S3 URL for a given object key.
  std::string BuildUrl(std::string_view key) const;

  // Build the canonical S3 host header.
  std::string BuildHost() const;

  // Sign a request with AWS Signature V4.
  void SignRequest(const std::string& method,
                   const std::string& url_path,
                   const std::string& query_string,
                   const std::string& payload_hash,
                   struct curl_slist** headers) const;

  // Low-level HTTP helpers.
  Status DoPut(const std::string& url, std::string_view data);
  Status DoGet(const std::string& url, std::string* out,
               size_t offset, size_t size);
  Status DoDelete(const std::string& url);

  S3Config cfg_;
  CURL* curl_;
  mutable std::mutex mu_;  // curl handle is not thread-safe
};

}  // namespace swordfs::storage
