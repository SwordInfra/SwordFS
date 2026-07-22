// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/S3DataEngine.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "utils/Logging.hpp"

namespace swordfs::storage {

// ── S3DataEngine ───────────────────────────────────────────────────────────

S3DataEngine::S3DataEngine(const S3Config& config)
    : cfg_(config), curl_(curl_easy_init()) {
  SWORDFS_LOG_INFO << "S3DataEngine: endpoint=" << cfg_.endpoint
                   << " bucket=" << cfg_.bucket;
}

S3DataEngine::~S3DataEngine() {
  if (curl_) curl_easy_cleanup(curl_);
}

Status S3DataEngine::Put(std::string_view key, std::string_view data) {
  std::string url = BuildUrl(key);
  return DoPut(url, data);
}

Status S3DataEngine::Get(std::string_view key, std::string* out,
                         size_t offset, size_t size) {
  std::string url = BuildUrl(key);
  return DoGet(url, out, offset, size);
}

Status S3DataEngine::Delete(std::string_view key) {
  std::string url = BuildUrl(key);
  return DoDelete(url);
}

// ── URL helpers ────────────────────────────────────────────────────────────

std::string S3DataEngine::BuildUrl(std::string_view key) const {
  // Build: https://<endpoint>/<bucket>/<prefix>/<key>
  std::string url = cfg_.endpoint;
  if (url.find("https://") != 0 && url.find("http://") != 0) {
    url = "https://" + url;
  }
  url += "/" + cfg_.bucket;
  if (!cfg_.prefix.empty()) {
    url += "/" + cfg_.prefix;
  }
  url += "/";
  url += key;
  return url;
}

std::string S3DataEngine::BuildHost() const {
  // Strip scheme from endpoint to get host.
  std::string host = cfg_.endpoint;
  if (host.find("https://") == 0) host = host.substr(8);
  else if (host.find("http://") == 0) host = host.substr(7);
  return host;
}

// ── AWS Signature V4 ───────────────────────────────────────────────────────

void S3DataEngine::SignRequest(const std::string& /*method*/,
                               const std::string& /*url_path*/,
                               const std::string& /*query_string*/,
                               const std::string& /*payload_hash*/,
                               struct curl_slist** /*headers*/) const {
  // TODO(#18): AWS SigV4 signing implementation.
  //
  // Steps:
  //   1. Build canonical request.
  //   2. Build string-to-sign.
  //   3. Calculate signing key.
  //   4. Add Authorization header.
}

// ── HTTP operations ────────────────────────────────────────────────────────

Status S3DataEngine::DoPut(const std::string& url, std::string_view data) {
  // TODO(#18): S3 PutObject with SigV4 auth.
  SWORDFS_LOG_DEBUG << "S3 Put: " << url << " (" << data.size() << " bytes)";
  return Status::OK();
}

Status S3DataEngine::DoGet(const std::string& url, std::string* out,
                           size_t offset, size_t size) {
  // TODO(#18): S3 GetObject with optional Range header.
  SWORDFS_LOG_DEBUG << "S3 Get: " << url
                    << " offset=" << offset << " size=" << size;
  return Status::OK();
}

Status S3DataEngine::DoDelete(const std::string& url) {
  // TODO(#18): S3 DeleteObject with SigV4 auth.
  SWORDFS_LOG_DEBUG << "S3 Delete: " << url;
  return Status::OK();
}

}  // namespace swordfs::storage
