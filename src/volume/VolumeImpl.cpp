// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include <folly/logging/xlog.h>

#include "chunk/IChunkOverwriteStrategy.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "storage/DataEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"
#include "utils/ExecutionDomain.hpp"

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
    return swordfs::metadata::MetaEngineRegistry::Instance().CreateInstance(url.scheme, meta_url, volume_name, out);
  } catch (const std::exception &error) {
    return Status::IOError("metadata engine initialization failed: " + std::string(error.what()));
  }
}

Status CreateDataEngine(std::string_view storage, std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  if (storage.empty()) {
    return Status::Malformed("volume storage backend is empty");
  }
  return swordfs::storage::DataEngineRegistry::Instance().CreateInstance(storage, out);
}

}  // namespace

VolumeImpl::VolumeImpl() {
  // Tests that inject engines without a format record still exercise the
  // currently implemented mechanism. Normal mounts replace this from disk.
  auto status = chunk::CreateChunkOverwriteStrategy("whole_object", 1, &chunk_overwrite_strategy_);
  CHECK(status.ok());
}
VolumeImpl::~VolumeImpl() {
  utils::ExpectInThreadDomain();
}

std::unique_ptr<VolumeImpl> VolumeImpl::instance_;

void VolumeImpl::Initialize() {
  utils::ExpectInThreadDomain();
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

const chunk::IChunkOverwriteStrategy *VolumeImpl::chunk_overwrite_strategy() const {
  // Unit tests can inject engines without formatting a volume. Preserve the
  // existing whole-object behavior for that fixture path.
  return chunk_overwrite_strategy_ != nullptr ? chunk_overwrite_strategy_.get()
                                              : &chunk::DefaultChunkOverwriteStrategy();
}

Status VolumeImpl::CreateFrom(const swordfs::config::ConfigCenter &cfg) {
  utils::ExpectInThreadDomain();
  config_.name = cfg.volume();
  config_.bucket = cfg.bucket_url();
  if (!config_.bucket.empty()) {
    utils::StorageUrl url;
    if (!utils::StorageUrl::Parse(config_.bucket, &url)) {
      return Status::InvalidArgument("invalid bucket URL: " + config_.bucket);
    }
    config_.storage = std::move(url.scheme);
  }
  config_.region = cfg.storage_region();
  if (config_.region.empty()) {
    config_.region = "auto";
  }
  config_.chunk_size = cfg.chunk_size();
  config_.chunk_overwrite_strategy = cfg.chunk_overwrite_strategy();
  config_.chunk_index_format_version = 1;

  auto status = chunk::CreateChunkOverwriteStrategy(config_.chunk_overwrite_strategy,
                                                    config_.chunk_index_format_version, &chunk_overwrite_strategy_);
  if (!status.ok()) {
    return status;
  }

  status = CreateMetaEngine(cfg.meta_url(), config_.name, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->BindChunkOverwriteStrategy(chunk_overwrite_strategy_.get());
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
  utils::ExpectInThreadDomain();
  config_.name = cfg.volume();

  auto status = CreateMetaEngine(cfg.meta_url(), config_.name, &meta_engine_);
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

  status = chunk::CreateChunkOverwriteStrategy(config_.chunk_overwrite_strategy, config_.chunk_index_format_version,
                                               &chunk_overwrite_strategy_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->BindChunkOverwriteStrategy(chunk_overwrite_strategy_.get());
  if (!status.ok()) {
    return status;
  }

  if (!config_.storage.empty()) {
    status = CreateDataEngine(config_.storage, &data_engine_);
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
  utils::ExpectInThreadDomain();
  data_engine_.reset();
  meta_engine_.reset();
  chunk_overwrite_strategy_.reset();
}

}  // namespace swordfs::volume
