// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include <folly/logging/xlog.h>

#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "storage/DataEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"

namespace swordfs::volume {
namespace {

Status CreateMetaEngine(std::string_view meta_url, std::string_view volume_name,
                         std::unique_ptr<swordfs::metadata::IMetaEngine> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("metadata engine output is null");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(meta_url, &url)) {
    return Status::InvalidArgument("invalid metadata URL: " + std::string(meta_url));
  }

  try {
    return swordfs::metadata::MetaEngineRegistry::Instance().Create(url.scheme, meta_url, volume_name, out);
  } catch (const std::exception &error) {
    return Status::IOError("metadata engine initialization failed: " + std::string(error.what()));
  }
}

Status CreateDataEngine(std::string_view bucket,
                        std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  if (bucket.empty()) {
    return Status::InvalidArgument("bucket URL is empty");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(bucket, &url)) {
    return Status::InvalidArgument("invalid bucket URL: " + std::string(bucket));
  }
  return swordfs::storage::DataEngineRegistry::Instance().Create(url.scheme, out);
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

  auto status = CreateMetaEngine(config_.meta_url, config_.name, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->Initialize();
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->FormatVolume(config_);
  if (!status.ok()) {
    return status;
  }

  return Status::OK();
}

Status VolumeImpl::LoadFrom(const swordfs::config::ConfigCenter &cfg) {
  config_.meta_url = cfg.meta_url();
  config_.name = cfg.volume();

  auto status = CreateMetaEngine(config_.meta_url, config_.name, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->Initialize();
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->LoadVolume(&config_);
  if (!status.ok()) {
    return status;
  }

  if (!config_.bucket.empty()) {
    status = CreateDataEngine(config_.bucket, &data_engine_);
    if (!status.ok()) {
      return status;
    }
    status = data_engine_->Initialize();
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
