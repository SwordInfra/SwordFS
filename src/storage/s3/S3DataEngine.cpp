// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/s3/S3DataEngine.hpp"

#include <aws/core/Aws.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>

#include "storage/StorageRegistry.hpp"
#include "utils/Logging.hpp"

namespace swordfs::storage {

DataEngineLimits S3DataEngine::Limits() const {
  DataEngineLimits limits;
  limits.max_chunk_size = 64 * 1024 * 1024;  // 64 MiB
  limits.supports_multipart = false;
  limits.supports_overwrite = false;
  return limits;
}

bool S3DataEngine::Head(std::string_view key, size_t* size) {
  Aws::S3::Model::HeadObjectRequest req;
  req.SetBucket(cfg_.bucket);
  req.SetKey(ObjectKey(key));

  std::lock_guard<std::mutex> lock(mu_);
  auto outcome = client_->HeadObject(req);
  if (!outcome.IsSuccess()) return false;
  if (size) *size = static_cast<size_t>(outcome.GetResult().GetContentLength());
  return true;
}

S3DataEngine::S3DataEngine(const S3Config& config) : cfg_(config) {
  Aws::Client::ClientConfiguration aws_cfg;
  aws_cfg.endpointOverride = cfg_.endpoint;
  aws_cfg.region = cfg_.region;

  client_ = std::make_unique<Aws::S3::S3Client>(std::move(aws_cfg));

  SWORDFS_LOG_INFO << "S3DataEngine: endpoint=" << cfg_.endpoint
                   << " bucket=" << cfg_.bucket;
}

Status S3DataEngine::Put(std::string_view key, std::string_view data) {
  Aws::S3::Model::PutObjectRequest req;
  req.SetBucket(cfg_.bucket);
  req.SetKey(ObjectKey(key));

  auto body = Aws::MakeShared<Aws::StringStream>("PutObject",
      std::string(data.data(), data.size()));
  req.SetBody(body);

  std::lock_guard<std::mutex> lock(mu_);
  auto outcome = client_->PutObject(req);
  if (!outcome.IsSuccess()) {
    SWORDFS_LOG_ERROR << "S3 PutObject failed: "
                      << outcome.GetError().GetMessage();
    return Status::Internal("S3 PutObject failed");
  }
  return Status::OK();
}

Status S3DataEngine::Get(std::string_view key, std::string* out,
                         size_t offset, size_t size) {
  Aws::S3::Model::GetObjectRequest req;
  req.SetBucket(cfg_.bucket);
  req.SetKey(ObjectKey(key));

  if (offset > 0 || size > 0) {
    std::string range = "bytes=" + std::to_string(offset) + "-";
    if (size > 0) range += std::to_string(offset + size - 1);
    req.SetRange(range);
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto outcome = client_->GetObject(req);
  if (!outcome.IsSuccess()) {
    SWORDFS_LOG_ERROR << "S3 GetObject failed: "
                      << outcome.GetError().GetMessage();
    return Status::Internal("S3 GetObject failed");
  }

  auto& stream = outcome.GetResult().GetBody();
  std::string result((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
  *out = std::move(result);
  return Status::OK();
}

Status S3DataEngine::Delete(std::string_view key) {
  Aws::S3::Model::DeleteObjectRequest req;
  req.SetBucket(cfg_.bucket);
  req.SetKey(ObjectKey(key));

  std::lock_guard<std::mutex> lock(mu_);
  auto outcome = client_->DeleteObject(req);
  if (!outcome.IsSuccess()) {
    SWORDFS_LOG_ERROR << "S3 DeleteObject failed: "
                      << outcome.GetError().GetMessage();
    return Status::Internal("S3 DeleteObject failed");
  }
  return Status::OK();
}

std::string S3DataEngine::ObjectKey(std::string_view key) const {
  if (cfg_.prefix.empty()) return std::string(key);
  return cfg_.prefix + "/" + std::string(key);
}

// ── Backend registration ───────────────────────────────────────────────

namespace {

// Register "s3" backend — uses a default config; the real S3Config is
// populated from ConfigCenter before the first mount.
static swordfs::storage::RegisterBackend kS3Backend(
    "s3", []() -> std::unique_ptr<swordfs::storage::IDataEngine> {
      return std::make_unique<swordfs::storage::S3DataEngine>(
          swordfs::storage::S3Config{});
    });

}  // anonymous namespace

}  // namespace swordfs::storage
