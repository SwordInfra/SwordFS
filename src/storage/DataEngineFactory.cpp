// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/DataEngineFactory.hpp"

#include <cstdlib>

#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"
#include "storage/s3/S3DataEngine.hpp"
#include "utils/FiberThreadPool.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs::storage {

utils::Status CreateDataEngine(
    const volume::VolumeConfig& vol,
    std::unique_ptr<IDataEngine>* out) {
  if (vol.bucket.empty()) {
    return utils::Status::InvalidArgument("bucket URL is empty");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(vol.bucket, &url)) {
    return utils::Status::InvalidArgument(
        "invalid bucket URL: " + vol.bucket);
  }

  if (url.scheme == "s3") {
    // bucket URL format: s3://<endpoint>/<bucket>[/<prefix>]
    S3Config s3_cfg;
    // Respect SWORDFS_S3_NO_SSL to allow plain HTTP connections
    // (e.g. against local MinIO in CI).
    const char* no_ssl = std::getenv("SWORDFS_S3_NO_SSL");
    const char* proto = (no_ssl && no_ssl[0] == '1') ? "http://" : "https://";
    s3_cfg.endpoint = std::string(proto) + url.host;
    // Use region from volume config (defaults to "auto").
    s3_cfg.region = vol.region;

    // First path segment is bucket, rest is prefix
    std::string path = url.path;
    if (!path.empty() && path[0] == '/') path = path.substr(1);

    auto slash = path.find('/');
    if (slash == std::string::npos) {
      s3_cfg.bucket = path;
    } else {
      s3_cfg.bucket = path.substr(0, slash);
      s3_cfg.prefix = path.substr(slash + 1);
    }

    if (s3_cfg.bucket.empty()) {
      return utils::Status::InvalidArgument(
          "bucket URL is missing bucket name. "
          "Expected format: s3://<endpoint>/<bucket>[/<prefix>], "
          "got: " + vol.bucket);
    }

    *out = std::make_unique<S3DataEngine>(s3_cfg);
    return utils::Status::OK();
  }
  return utils::Status::InvalidArgument(
      "unknown data storage scheme: " + std::string(url.scheme));
}

}  // namespace swordfs::storage
