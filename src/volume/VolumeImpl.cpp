// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include <folly/logging/xlog.h>

#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineFactory.hpp"
#include "storage/DataEngineFactory.hpp"
#include "storage/IDataEngine.hpp"
#include "storage/s3/S3DataEngine.hpp"

namespace swordfs::volume {

VolumeImpl::VolumeImpl() = default;
VolumeImpl::~VolumeImpl() = default;

std::unique_ptr<VolumeImpl> VolumeImpl::instance_;

void VolumeImpl::Initialize() {
  instance_ = std::make_unique<VolumeImpl>();
}

VolumeImpl &VolumeImpl::Instance() {
  return *instance_;
}

void VolumeImpl::set_meta_engine(
    std::unique_ptr<swordfs::metadata::IMetaEngine> meta) {
  meta_engine_ = std::move(meta);
}

void VolumeImpl::set_data_engine(
    std::unique_ptr<swordfs::storage::IDataEngine> data) {
  data_engine_ = std::move(data);
}

Status VolumeImpl::CreateFrom(const swordfs::config::ConfigCenter &cfg) {
  config_.meta_url = cfg.meta_url();
  if (!swordfs::metadata::IsMemoryMode(config_.meta_url)) {
    return Status::InvalidArgument(
        "unsupported metadata engine: " + config_.meta_url);
  }

  config_.name = cfg.volume();
  config_.storage = cfg.storage_backend();
  config_.bucket = cfg.bucket_url();
  config_.region = cfg.storage_region();
  if (config_.region.empty()) {
    config_.region = "auto";
  }
  config_.chunk_size = cfg.chunk_size();

  const std::string &config_path = cfg.volume_config_path();
  if (!config_path.empty()) {
    if (VolumeConfig::ConfigFileExists(config_path)) {
      return Status::AlreadyExists("volume already exists at " + config_path);
    }
    auto status = config_.WriteToFile(config_path);
    if (!status.ok()) {
      return status;
    }
  }

  return Status::OK();
}

Status VolumeImpl::LoadFrom(const swordfs::config::ConfigCenter &cfg) {
  auto status = config_.ReadFromFile(cfg.volume_config_path());
  if (!status.ok()) {
    return status;
  }

  // Create engines.
  status = swordfs::metadata::CreateMetaEngine(config_.meta_url, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  if (!config_.bucket.empty()) {
    status = storage::CreateDataEngine(config_, &data_engine_);
    if (!status.ok()) {
      return status;
    }
  }

  return Status::OK();
}

void VolumeImpl::Shutdown() {
  data_engine_.reset();
  meta_engine_.reset();
}

}  // namespace swordfs::volume
