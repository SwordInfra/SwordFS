// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/s3/S3DataEngine.hpp"

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include "config/ConfigCenter.hpp"
#include "storage/StorageRegistry.hpp"
#include "utils/FiberThreadPool.hpp"
#include <folly/logging/xlog.h>
#include "utils/Logging.hpp"

namespace swordfs::storage {

S3DataEngine::~S3DataEngine() = default;

// ────────────────────────────────────────────────────────────────
// AWS SDK lifetime — initialised on first S3DataEngine creation
// and shut down at process exit.
// ────────────────────────────────────────────────────────────────

namespace {
void EnsureAwsSdkInit() {
  static const struct AwsSdkGuard {
    AwsSdkGuard() {
      // AWS SDK credential-resolution order:
      //   1. AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY env vars
      //   2. ~/.aws/credentials
      //   3. EC2 / ECS metadata service (169.254.169.254)
      //
      // When neither environment variable is set, the SDK falls
      // through to EC2 metadata, which adds ~10 s of TCP connect
      // timeout on non-EC2 machines.  Disable that path explicitly.
      if (!std::getenv("AWS_ACCESS_KEY_ID") ||
          !std::getenv("AWS_SECRET_ACCESS_KEY")) {
        setenv("AWS_EC2_METADATA_DISABLED", "true", /*overwrite=*/0);
      }
      Aws::SDKOptions opts;
      Aws::InitAPI(opts);
    }
    ~AwsSdkGuard() {
      Aws::SDKOptions opts;
      Aws::ShutdownAPI(opts);
    }
  } guard;
  (void)guard;
}
}  // namespace

S3DataEngine::S3DataEngine(const S3Config &config)
    : cfg_(config),
      pool_(std::make_shared<utils::FiberThreadPool>(
          static_cast<size_t>(config::ConfigCenter::Instance().storage_async_threads()))) {
  EnsureAwsSdkInit();
  Aws::S3::S3ClientConfiguration aws_cfg;
  aws_cfg.endpointOverride = cfg_.endpoint;
  // Use path-style addressing by default (http://<host>/<bucket>/<key>)
  // for MinIO and other S3-compatible stores.  Set
  // SWORDFS_S3_VIRTUAL_HOSTED=1 to switch to virtual-hosted style
  // (http://<bucket>.<host>/<key>) for real AWS S3.
  const char* vh = std::getenv("SWORDFS_S3_VIRTUAL_HOSTED");
  aws_cfg.useVirtualAddressing = (vh && vh[0] == '1');
  // Only override region if explicitly provided; otherwise let the SDK
  // resolve it via the default chain (AWS_DEFAULT_REGION env var,
  // ~/.aws/config, IAM role, etc.).
  if (!cfg_.region.empty()) {
    aws_cfg.region = cfg_.region;
  }
  client_ = std::make_unique<Aws::S3::S3Client>(std::move(aws_cfg));

  SWORDFS_LOG_INFO << "S3DataEngine: endpoint=" << cfg_.endpoint
                   << " bucket=" << cfg_.bucket;
}

DataEngineLimits S3DataEngine::Limits() const {
  DataEngineLimits limits;
  limits.supports_multipart = false;
  return limits;
}

bool S3DataEngine::Head(std::string_view key, size_t *size) {
  return pool_->Run([this, key, size] {
    Aws::S3::Model::HeadObjectRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(ObjectKey(key));

    auto outcome = client_->HeadObject(req);
    if (!outcome.IsSuccess()) return false;
    if (size) *size = static_cast<size_t>(outcome.GetResult().GetContentLength());
    return true;
  });
}

Status S3DataEngine::Put(std::string_view key, std::string_view data) {
  SWORDFS_LOG_DEBUG << "S3 Put: key=" << key << " size=" << data.size();
  try {
    return pool_->Run([this, key, data] {
      Aws::S3::Model::PutObjectRequest req;
      req.SetBucket(cfg_.bucket);
      req.SetKey(ObjectKey(key));

      auto body = Aws::MakeShared<Aws::StringStream>(
          "PutObject", std::string(data.data(), data.size()));
      req.SetBody(body);

      SWORDFS_LOG_DEBUG << "S3 PutObject sending: bucket=" << cfg_.bucket
                        << " key=" << ObjectKey(key);
      auto outcome = client_->PutObject(req);
      if (!outcome.IsSuccess()) {
        SWORDFS_LOG_ERROR << "S3 PutObject failed: "
                          << outcome.GetError().GetMessage();
        return Status::Internal("S3 PutObject failed");
      }
      SWORDFS_LOG_DEBUG << "S3 PutObject OK";
      return Status::OK();
    });
  } catch (const std::exception& e) {
    SWORDFS_LOG_ERROR << "S3 Put EXCEPTION: " << e.what();
    return Status::Internal(std::string("S3 Put crashed: ") + e.what());
  } catch (...) {
    SWORDFS_LOG_ERROR << "S3 Put unknown EXCEPTION";
    return Status::Internal("S3 Put crashed: unknown exception");
  }
}

Status S3DataEngine::Get(std::string_view key, std::string *out,
                         size_t offset, size_t size) {
  SWORDFS_LOG_DEBUG << "S3 Get: key=" << key << " off=" << offset
                    << " size=" << size;
  try {
    return pool_->Run(
        [this, key, out, offset, size] {
      Aws::S3::Model::GetObjectRequest req;
      req.SetBucket(cfg_.bucket);
      req.SetKey(ObjectKey(key));

      if (offset > 0 || size > 0) {
        std::string range = "bytes=" + std::to_string(offset) + "-";
        if (size > 0) range += std::to_string(offset + size - 1);
        req.SetRange(range);
      }

      auto outcome = client_->GetObject(req);
      if (!outcome.IsSuccess()) {
        if (outcome.GetError().GetResponseCode() ==
            Aws::Http::HttpResponseCode::NOT_FOUND) {
          SWORDFS_LOG_WARN << "S3 GetObject: chunk not found: "
                           << ObjectKey(key);
          return Status::NotFound("chunk not found");
        }
        SWORDFS_LOG_ERROR << "S3 GetObject failed: "
                          << outcome.GetError().GetMessage();
        return Status::Internal("S3 GetObject failed");
      }

      auto &stream = outcome.GetResult().GetBody();
      std::string result((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
      *out = std::move(result);
      return Status::OK();
    });
  } catch (const std::exception& e) {
    SWORDFS_LOG_ERROR << "S3 Get EXCEPTION: " << e.what();
    return Status::Internal(std::string("S3 Get crashed: ") + e.what());
  } catch (...) {
    SWORDFS_LOG_ERROR << "S3 Get unknown EXCEPTION";
    return Status::Internal("S3 Get crashed: unknown exception");
  }
}

Status S3DataEngine::Delete(std::string_view key) {
  return pool_->Run([this, key] {
    Aws::S3::Model::DeleteObjectRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(ObjectKey(key));

    auto outcome = client_->DeleteObject(req);
    if (!outcome.IsSuccess()) {
      SWORDFS_LOG_ERROR << "S3 DeleteObject failed: "
                        << outcome.GetError().GetMessage();
      return Status::Internal("S3 DeleteObject failed");
    }
    return Status::OK();
  });
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
