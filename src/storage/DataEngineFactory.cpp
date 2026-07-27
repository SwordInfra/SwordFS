// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/DataEngineFactory.hpp"

#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"
#include "storage/s3/S3DataEngine.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs::storage {

std::unique_ptr<IDataEngine> CreateDataEngine(
    const volume::VolumeConfig& vol) {
  if (vol.bucket.empty()) return nullptr;

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(vol.bucket, &url)) {
    SWORDFS_LOG_ERROR << "Invalid bucket URL: " << vol.bucket;
    return nullptr;
  }

  if (url.scheme == "s3") {
    // bucket URL format: s3://<endpoint>/<bucket>[/<prefix>]
    S3Config s3_cfg;
    s3_cfg.endpoint = "https://" + url.host;
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

    return std::make_unique<S3DataEngine>(s3_cfg);
  }
  SWORDFS_LOG_ERROR << "Unknown data storage scheme: " << url.scheme;
  return nullptr;
}

}  // namespace swordfs::storage
