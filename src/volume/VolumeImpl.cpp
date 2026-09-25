// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include <folly/logging/xlog.h>

#include <algorithm>

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

Status CreateDataEngine(std::string_view storage, const swordfs::storage::DataEngineOptions &options,
                        std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  if (storage.empty()) {
    return Status::Malformed("volume storage backend is empty");
  }
  return swordfs::storage::DataEngineRegistry::Instance().CreateInstance(storage, options, out);
}

}  // namespace

VolumeImpl::VolumeImpl() {
  // Before format/load binds persisted configuration, the runtime uses the
  // current default overwrite mechanism.
  auto status = chunk::CreateChunkOverwriteStrategy(metadata::ChunkOverwriteMechanism::kWholeObject, 1,
                                                    &chunk_overwrite_strategy_);
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

const chunk::IChunkOverwriteStrategy *VolumeImpl::chunk_overwrite_strategy() const {
  return chunk_overwrite_strategy_.get();
}

Status VolumeImpl::CreateFrom(const config::ConfigCenter &config) {
  metadata::ChunkOverwriteMechanism mechanism;
  auto status = metadata::ParseChunkOverwriteMechanism(config.chunk_overwrite_strategy(), &mechanism);
  if (!status.ok()) {
    return status;
  }
  return CreateFrom(FormatOptions{
      .name = config.volume(),
      .meta_url = config.meta_url(),
      .bucket = config.bucket_url(),
      .region = config.storage_region(),
      .chunk_size = config.chunk_size(),
      .chunk_overwrite_mechanism = mechanism,
  });
}

Status VolumeImpl::CreateFrom(const FormatOptions &options) {
  utils::ExpectInThreadDomain();
  config_.name = options.name;
  config_.bucket = options.bucket;
  if (!config_.bucket.empty()) {
    utils::StorageUrl url;
    if (!utils::StorageUrl::Parse(config_.bucket, &url)) {
      return Status::InvalidArgument("invalid bucket URL: " + config_.bucket);
    }
    config_.storage = std::move(url.scheme);
  }
  config_.region = options.region;
  if (config_.region.empty()) {
    config_.region = "auto";
  }
  config_.chunk_size = options.chunk_size;
  config_.chunk_overwrite_mechanism = options.chunk_overwrite_mechanism;
  config_.chunk_index_format_version = 1;

  auto status = chunk::CreateChunkOverwriteStrategy(config_.chunk_overwrite_mechanism,
                                                    config_.chunk_index_format_version, &chunk_overwrite_strategy_);
  if (!status.ok()) {
    return status;
  }

  status = CreateMetaEngine(options.meta_url, config_.name, &meta_engine_);
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

Status VolumeImpl::LoadFrom(const config::ConfigCenter &config) {
  return LoadFrom(MountOptions{
      .name = config.volume(),
      .meta_url = config.meta_url(),
      .storage_thread_count = static_cast<size_t>(std::max(1, config.storage_thread_count())),
  });
}

Status VolumeImpl::LoadFrom(const MountOptions &options) {
  utils::ExpectInThreadDomain();
  config_.name = options.name;

  auto status = CreateMetaEngine(options.meta_url, config_.name, &meta_engine_);
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

  status = chunk::CreateChunkOverwriteStrategy(config_.chunk_overwrite_mechanism, config_.chunk_index_format_version,
                                               &chunk_overwrite_strategy_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->BindChunkOverwriteStrategy(chunk_overwrite_strategy_.get());
  if (!status.ok()) {
    return status;
  }

  if (!config_.storage.empty()) {
    status = CreateDataEngine(config_.storage,
                              swordfs::storage::DataEngineOptions{
                                  .location = config_.bucket,
                                  .region = config_.region,
                                  .worker_count = options.storage_thread_count,
                              },
                              &data_engine_);
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
