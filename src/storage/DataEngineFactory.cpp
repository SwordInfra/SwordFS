// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/DataEngineFactory.hpp"

#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"
#include "storage/s3/S3DataEngine.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs::storage {

utils::Status CreateDataEngine(
    const volume::VolumeConfig &vol,
    std::unique_ptr<IDataEngine> *out) {
  if (vol.bucket.empty()) {
    return utils::Status::InvalidArgument("bucket URL is empty");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(vol.bucket, &url)) {
    return utils::Status::InvalidArgument(
        "invalid bucket URL: " + vol.bucket);
  }

  if (url.scheme == "s3") {
    auto engine = std::make_unique<S3DataEngine>();
    auto status = engine->Initialize();
    if (!status.ok()) {
      return status;
    }
    *out = std::move(engine);
    return utils::Status::OK();
  }
  return utils::Status::InvalidArgument(
      "unknown data storage scheme: " + std::string(url.scheme));
}

}  // namespace swordfs::storage