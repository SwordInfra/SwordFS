// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include "config/ConfigCenter.hpp"
#include "fuse/Vfs.hpp"
#include "metadata/Meta.hpp"
#include "storage/DataEngineFactory.hpp"
#include "storage/IDataEngine.hpp"

namespace swordfs::volume {

Status VolumeImpl::CreateFrom(const swordfs::config::ConfigCenter& cfg) {
  config_.meta_url = cfg.meta_url();
  if (!swordfs::metadata::IsMemoryMode(config_.meta_url)) {
    return Status::InvalidArgument(
        "unsupported metadata engine: " + config_.meta_url);
  }

  config_.name = cfg.volume();
  config_.storage = cfg.storage_backend();
  config_.bucket = cfg.bucket_url();
  config_.region = cfg.storage_region();
  if (config_.region.empty()) config_.region = "auto";

  const std::string& config_path = cfg.volume_config_path();
  if (!config_path.empty()) {
    auto status = config_.WriteToFile(config_path);
    if (!status.ok()) return status;
  }

  return Status::OK();
}

Status VolumeImpl::LoadFrom(const swordfs::config::ConfigCenter& cfg) {
  if (!swordfs::metadata::IsMemoryMode(cfg.meta_url())) {
    return Status::NotSupported(
        "unsupported metadata engine: " + cfg.meta_url());
  }

  auto status = config_.ReadFromFile(cfg.volume_config_path());
  if (!status.ok()) return status;

  auto engine = swordfs::storage::CreateDataEngine(config_);
  if (engine) {
    swordfs::fuse::VfsHookFactory::set_data_engine(std::move(engine));
  } else if (!config_.bucket.empty()) {
    return Status::InvalidArgument(
        "failed to create data engine for " + config_.bucket);
  }

  return Status::OK();
}

}  // namespace swordfs::volume
