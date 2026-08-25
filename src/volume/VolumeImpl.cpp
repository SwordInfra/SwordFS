// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include <folly/logging/xlog.h>

#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"
#include "storage/s3/S3DataEngine.hpp"

namespace swordfs::volume {
namespace {

Status CreateMetaEngine(std::string_view meta_url, std::unique_ptr<swordfs::metadata::IMetaEngine> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("metadata engine output is null");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(meta_url, &url)) {
    return Status::InvalidArgument("invalid metadata URL: " + std::string(meta_url));
  }

  try {
    return swordfs::metadata::MetaEngineRegistry::Instance().Create(url.scheme, meta_url, out);
  } catch (const std::exception &error) {
    return Status::IOError("metadata engine initialization failed: " + std::string(error.what()));
  }
}

Status CreateDataEngine(const VolumeConfig &config,
                        std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("data engine output is null");
  }
  if (config.bucket.empty()) {
    return Status::InvalidArgument("bucket URL is empty");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(config.bucket, &url)) {
    return Status::InvalidArgument("invalid bucket URL: " + config.bucket);
  }
  if (url.scheme != "s3") {
    return Status::NotSupported("unknown data storage scheme: " + std::string(url.scheme));
  }

  *out = std::make_unique<swordfs::storage::S3DataEngine>();
  return Status::OK();
}

}  // namespace

VolumeImpl::VolumeImpl() = default;
VolumeImpl::~VolumeImpl() = default;

std::unique_ptr<VolumeImpl> VolumeImpl::instance_;

void VolumeImpl::Initialize() {
  instance_ = std::make_unique<VolumeImpl>();
}

VolumeImpl &VolumeImpl::Instance() {
  return *instance_;
}

void VolumeImpl::set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine> meta) {
  meta_engine_ = std::move(meta);
}

void VolumeImpl::set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine> data) {
  data_engine_ = std::move(data);
}

Status VolumeImpl::CreateFrom(const swordfs::config::ConfigCenter &cfg) {
  config_.meta_url = cfg.meta_url();
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

  auto status = CreateMetaEngine(config_.meta_url, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->Initialize();
  if (!status.ok()) {
    meta_engine_.reset();
    return status;
  }
  status = meta_engine_->Format();
  if (!status.ok()) {
    meta_engine_.reset();
    return status;
  }

  return Status::OK();
}

Status VolumeImpl::LoadFrom(const swordfs::config::ConfigCenter &cfg) {
  auto status = config_.ReadFromFile(cfg.volume_config_path());
  if (!status.ok()) {
    return status;
  }

  // Create and initialize both engines as part of the volume lifecycle.
  status = CreateMetaEngine(config_.meta_url, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->Initialize();
  if (!status.ok()) {
    meta_engine_.reset();
    return status;
  }
  status = meta_engine_->Validate();
  if (!status.ok()) {
    return status;
  }

  if (!config_.bucket.empty()) {
    status = CreateDataEngine(config_, &data_engine_);
    if (!status.ok()) {
      return status;
    }
    status = data_engine_->Initialize();
    if (!status.ok()) {
      data_engine_.reset();
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
